#include <cerrno>
#include <cstdarg>

#include "node_errors.h"
#include "node_internals.h"
#ifdef NODE_REPORT
#include "node_report.h"
#endif
#include "node_process.h"
#include "node_v8_platform-inl.h"
#include "util-inl.h"

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace node {

using errors::TryCatchScope;
using v8::Boolean;
using v8::Context;
using v8::Exception;
using v8::Function;
using v8::FunctionCallbackInfo;
using v8::HandleScope;
using v8::Int32;
using v8::Isolate;
using v8::Just;
using v8::Local;
using v8::Maybe;
using v8::MaybeLocal;
using v8::Message;
using v8::Number;
using v8::Object;
using v8::ScriptOrigin;
using v8::StackFrame;
using v8::StackTrace;
using v8::String;
using v8::Undefined;
using v8::Value;

bool IsExceptionDecorated(Environment* env, Local<Value> er) {
  if (!er.IsEmpty() && er->IsObject()) {
    Local<Object> err_obj = er.As<Object>();
    auto maybe_value =
        err_obj->GetPrivate(env->context(), env->decorated_private_symbol());
    Local<Value> decorated;
    return maybe_value.ToLocal(&decorated) && decorated->IsTrue();
  }
  return false;
}

namespace per_process {
static Mutex tty_mutex;
}  // namespace per_process

static const int kMaxErrorSourceLength = 1024;

static std::string GetErrorSource(Isolate* isolate,
                                  Local<Context> context,
                                  Local<Message> message,
                                  bool* added_exception_line) {
  MaybeLocal<String> source_line_maybe = message->GetSourceLine(context);
  node::Utf8Value encoded_source(isolate, source_line_maybe.ToLocalChecked());
  std::string sourceline(*encoded_source, encoded_source.length());

  if (sourceline.find("node-do-not-add-exception-line") != std::string::npos) {
    *added_exception_line = false;
    return sourceline;
  }

  // Because of how node modules work, all scripts are wrapped with a
  // "function (module, exports, __filename, ...) {"
  // to provide script local variables.
  //
  // When reporting errors on the first line of a script, this wrapper
  // function is leaked to the user. There used to be a hack here to
  // truncate off the first 62 characters, but it caused numerous other
  // problems when vm.runIn*Context() methods were used for non-module
  // code.
  //
  // If we ever decide to re-instate such a hack, the following steps
  // must be taken:
  //
  // 1. Pass a flag around to say "this code was wrapped"
  // 2. Update the stack frame output so that it is also correct.
  //
  // It would probably be simpler to add a line rather than add some
  // number of characters to the first line, since V8 truncates the
  // sourceline to 78 characters, and we end up not providing very much
  // useful debugging info to the user if we remove 62 characters.

  // Print (filename):(line number): (message).
  ScriptOrigin origin = message->GetScriptOrigin();
  node::Utf8Value filename(isolate, message->GetScriptResourceName());
  const char* filename_string = *filename;
  int linenum = message->GetLineNumber(context).FromJust();

  int script_start = (linenum - origin.ResourceLineOffset()->Value()) == 1
                         ? origin.ResourceColumnOffset()->Value()
                         : 0;
  int start = message->GetStartColumn(context).FromMaybe(0);
  int end = message->GetEndColumn(context).FromMaybe(0);
  if (start >= script_start) {
    CHECK_GE(end, start);
    start -= script_start;
    end -= script_start;
  }

  int max_off = kMaxErrorSourceLength - 2;

  char buf[kMaxErrorSourceLength];
  int off = snprintf(buf,
                     kMaxErrorSourceLength,
                     "%s:%i\n%s\n",
                     filename_string,
                     linenum,
                     sourceline.c_str());
  CHECK_GE(off, 0);
  if (off > max_off) {
    off = max_off;
  }

  // Print wavy underline (GetUnderline is deprecated).
  for (int i = 0; i < start; i++) {
    if (sourceline[i] == '\0' || off >= max_off) {
      break;
    }
    CHECK_LT(off, max_off);
    buf[off++] = (sourceline[i] == '\t') ? '\t' : ' ';
  }
  for (int i = start; i < end; i++) {
    if (sourceline[i] == '\0' || off >= max_off) {
      break;
    }
    CHECK_LT(off, max_off);
    buf[off++] = '^';
  }
  CHECK_LE(off, max_off);
  buf[off] = '\n';
  buf[off + 1] = '\0';

  *added_exception_line = true;
  return std::string(buf);
}

void PrintStackTrace(Isolate* isolate, Local<StackTrace> stack) {
  for (int i = 0; i < stack->GetFrameCount(); i++) {
    Local<StackFrame> stack_frame = stack->GetFrame(isolate, i);
    node::Utf8Value fn_name_s(isolate, stack_frame->GetFunctionName());
    node::Utf8Value script_name(isolate, stack_frame->GetScriptName());
    const int line_number = stack_frame->GetLineNumber();
    const int column = stack_frame->GetColumn();

    if (stack_frame->IsEval()) {
      if (stack_frame->GetScriptId() == Message::kNoScriptIdInfo) {
        fprintf(stderr, "    at [eval]:%i:%i\n", line_number, column);
      } else {
        fprintf(stderr,
                "    at [eval] (%s:%i:%i)\n",
                *script_name,
                line_number,
                column);
      }
      break;
    }

    if (fn_name_s.length() == 0) {
      fprintf(stderr, "    at %s:%i:%i\n", *script_name, line_number, column);
    } else {
      fprintf(stderr,
              "    at %s (%s:%i:%i)\n",
              *fn_name_s,
              *script_name,
              line_number,
              column);
    }
  }
  fflush(stderr);
}

void PrintException(Isolate* isolate,
                    Local<Context> context,
                    Local<Value> err,
                    Local<Message> message) {
  node::Utf8Value reason(isolate,
                         err->ToDetailString(context)
                             .FromMaybe(Local<String>()));
  bool added_exception_line = false;
  std::string source =
      GetErrorSource(isolate, context, message, &added_exception_line);
  fprintf(stderr, "%s\n", source.c_str());
  fprintf(stderr, "%s\n", *reason);

  Local<v8::StackTrace> stack = message->GetStackTrace();
  if (!stack.IsEmpty()) PrintStackTrace(isolate, stack);
}

void PrintCaughtException(Isolate* isolate,
                          Local<Context> context,
                          const v8::TryCatch& try_catch) {
  CHECK(try_catch.HasCaught());
  PrintException(isolate, context, try_catch.Exception(), try_catch.Message());
}

void AppendExceptionLine(Environment* env,
                         Local<Value> er,
                         Local<Message> message,
                         enum ErrorHandlingMode mode) {
  if (message.IsEmpty()) return;

  HandleScope scope(env->isolate());
  Local<Object> err_obj;
  if (!er.IsEmpty() && er->IsObject()) {
    err_obj = er.As<Object>();
  }

  bool added_exception_line = false;
  std::string source = GetErrorSource(
      env->isolate(), env->context(), message, &added_exception_line);
  if (!added_exception_line) {
    return;
  }
  MaybeLocal<Value> arrow_str = ToV8Value(env->context(), source);

  const bool can_set_arrow = !arrow_str.IsEmpty() && !err_obj.IsEmpty();
  // If allocating arrow_str failed, print it out. There's not much else to do.
  // If it's not an error, but something needs to be printed out because
  // it's a fatal exception, also print it out from here.
  // Otherwise, the arrow property will be attached to the object and handled
  // by the caller.
  if (!can_set_arrow || (mode == FATAL_ERROR && !err_obj->IsNativeError())) {
    if (env->printed_error()) return;
    Mutex::ScopedLock lock(per_process::tty_mutex);
    env->set_printed_error(true);

    ResetStdio();
    PrintErrorString("\n%s", source.c_str());
    return;
  }

  CHECK(err_obj
            ->SetPrivate(env->context(),
                         env->arrow_message_private_symbol(),
                         arrow_str.ToLocalChecked())
            .FromMaybe(false));
}

[[noreturn]] void Abort() {
  DumpBacktrace(stderr);
  fflush(stderr);
  ABORT_NO_BACKTRACE();
}

[[noreturn]] void Assert(const AssertionInfo& info) {
  char name[1024];
  GetHumanReadableProcessName(&name);

  fprintf(stderr,
          "%s: %s:%s%s Assertion `%s' failed.\n",
          name,
          info.file_line,
          info.function,
          *info.function ? ":" : "",
          info.message);
  fflush(stderr);

  Abort();
}

enum class EnhanceFatalException { kEnhance, kDontEnhance };

/**
 * Report the exception to the inspector, then print it to stderr.
 * This should only be used when the Node.js instance is about to exit
 * (i.e. this should be followed by a env->Exit() or an Abort()).
 *
 * Use enhance_stack = EnhanceFatalException::kDontEnhance
 * when it's unsafe to call into JavaScript.
 */
static void ReportFatalException(Environment* env,
                                 Local<Value> error,
                                 Local<Message> message,
                                 EnhanceFatalException enhance_stack) {
  Isolate* isolate = env->isolate();
  CHECK(!error.IsEmpty());
  CHECK(!message.IsEmpty());
  HandleScope scope(isolate);

  AppendExceptionLine(env, error, message, FATAL_ERROR);

  auto report_to_inspector = [&]() {
#if HAVE_INSPECTOR
    env->inspector_agent()->ReportUncaughtException(error, message);
#endif
  };

  Local<Value> arrow;
  Local<Value> stack_trace;
  bool decorated = IsExceptionDecorated(env, error);

  if (!error->IsObject()) {  // We can only enhance actual errors.
    report_to_inspector();
    stack_trace = Undefined(isolate);
    // If error is not an object, AppendExceptionLine() has already print the
    // source line and the arrow to stderr.
    // TODO(joyeecheung): move that side effect out of AppendExceptionLine().
    // It is done just to preserve the source line as soon as possible.
  } else {
    Local<Object> err_obj = error.As<Object>();

    auto enhance_with = [&](Local<Function> enhancer) {
      Local<Value> enhanced;
      Local<Value> argv[] = {err_obj};
      if (!enhancer.IsEmpty() &&
          enhancer
              ->Call(env->context(), Undefined(isolate), arraysize(argv), argv)
              .ToLocal(&enhanced)) {
        stack_trace = enhanced;
      }
    };

    switch (enhance_stack) {
      case EnhanceFatalException::kEnhance: {
        enhance_with(env->enhance_fatal_stack_before_inspector());
        report_to_inspector();
        enhance_with(env->enhance_fatal_stack_after_inspector());
        break;
      }
      case EnhanceFatalException::kDontEnhance: {
        USE(err_obj->Get(env->context(), env->stack_string())
                .ToLocal(&stack_trace));
        report_to_inspector();
        break;
      }
      default:
        UNREACHABLE();
    }

    arrow =
        err_obj->GetPrivate(env->context(), env->arrow_message_private_symbol())
            .ToLocalChecked();
  }

  node::Utf8Value trace(env->isolate(), stack_trace);

  // range errors have a trace member set to undefined
  if (trace.length() > 0 && !stack_trace->IsUndefined()) {
    if (arrow.IsEmpty() || !arrow->IsString() || decorated) {
      PrintErrorString("%s\n", *trace);
    } else {
      node::Utf8Value arrow_string(env->isolate(), arrow);
      PrintErrorString("%s\n%s\n", *arrow_string, *trace);
    }
  } else {
    // this really only happens for RangeErrors, since they're the only
    // kind that won't have all this info in the trace, or when non-Error
    // objects are thrown manually.
    MaybeLocal<Value> message;
    MaybeLocal<Value> name;

    if (error->IsObject()) {
      Local<Object> err_obj = error.As<Object>();
      message = err_obj->Get(env->context(), env->message_string());
      name = err_obj->Get(env->context(), env->name_string());
    }

    if (message.IsEmpty() || message.ToLocalChecked()->IsUndefined() ||
        name.IsEmpty() || name.ToLocalChecked()->IsUndefined()) {
      // Not an error object. Just print as-is.
      String::Utf8Value message(env->isolate(), error);

      PrintErrorString("%s\n",
                       *message ? *message : "<toString() threw exception>");
    } else {
      node::Utf8Value name_string(env->isolate(), name.ToLocalChecked());
      node::Utf8Value message_string(env->isolate(), message.ToLocalChecked());

      if (arrow.IsEmpty() || !arrow->IsString() || decorated) {
        PrintErrorString("%s: %s\n", *name_string, *message_string);
      } else {
        node::Utf8Value arrow_string(env->isolate(), arrow);
        PrintErrorString(
            "%s\n%s: %s\n", *arrow_string, *name_string, *message_string);
      }
    }

    if (!env->options()->trace_uncaught) {
      PrintErrorString("(Use `node --trace-uncaught ...` to show "
                       "where the exception was thrown)\n");
    }
  }

  if (env->options()->trace_uncaught) {
    Local<StackTrace> trace = message->GetStackTrace();
    if (!trace.IsEmpty()) {
      PrintErrorString("Thrown at:\n");
      PrintStackTrace(env->isolate(), trace);
    }
  }

  fflush(stderr);
}

void PrintErrorString(const char* format, ...) {
  va_list ap;
  va_start(ap, format);
#ifdef _WIN32
  HANDLE stderr_handle = GetStdHandle(STD_ERROR_HANDLE);

  // Check if stderr is something other than a tty/console
  if (stderr_handle == INVALID_HANDLE_VALUE || stderr_handle == nullptr ||
      uv_guess_handle(_fileno(stderr)) != UV_TTY) {
    vfprintf(stderr, format, ap);
    va_end(ap);
    return;
  }

  // Fill in any placeholders
  int n = _vscprintf(format, ap);
  std::vector<char> out(n + 1);
  vsprintf(out.data(), format, ap);

  // Get required wide buffer size
  n = MultiByteToWideChar(CP_UTF8, 0, out.data(), -1, nullptr, 0);

  std::vector<wchar_t> wbuf(n);
  MultiByteToWideChar(CP_UTF8, 0, out.data(), -1, wbuf.data(), n);

  // Don't include the null character in the output
  CHECK_GT(n, 0);
  WriteConsoleW(stderr_handle, wbuf.data(), n - 1, nullptr, nullptr);
#elif defined(__ANDROID__)
  __android_log_vprint(ANDROID_LOG_ERROR, "nodejs", format, ap);
#else
  vfprintf(stderr, format, ap);
#endif
  va_end(ap);
}

[[noreturn]] void FatalError(const char* location, const char* message) {
  OnFatalError(location, message);
  // to suppress compiler warning
  ABORT();
}

void OnFatalError(const char* location, const char* message) {
  if (location) {
    PrintErrorString("FATAL ERROR: %s %s\n", location, message);
  } else {
    PrintErrorString("FATAL ERROR: %s\n", message);
  }
#ifdef NODE_REPORT
  Isolate* isolate = Isolate::GetCurrent();
  Environment* env = Environment::GetCurrent(isolate);
  if (env == nullptr || env->isolate_data()->options()->report_on_fatalerror) {
    report::TriggerNodeReport(
        isolate, env, message, "FatalError", "", Local<String>());
  }
#endif  // NODE_REPORT
  fflush(stderr);
  ABORT();
}

namespace errors {

TryCatchScope::~TryCatchScope() {
  if (HasCaught() && !HasTerminated() && mode_ == CatchMode::kFatal) {
    HandleScope scope(env_->isolate());
    Local<v8::Value> exception = Exception();
    Local<v8::Message> message = Message();
    EnhanceFatalException enhance = CanContinue() ?
        EnhanceFatalException::kEnhance : EnhanceFatalException::kDontEnhance;
    if (message.IsEmpty())
      message = Exception::CreateMessage(env_->isolate(), exception);
    ReportFatalException(env_, exception, message, enhance);
    env_->Exit(7);
  }
}

const char* errno_string(int errorno) {
#define ERRNO_CASE(e)                                                          \
  case e:                                                                      \
    return #e;
  switch (errorno) {
#ifdef EACCES
    ERRNO_CASE(EACCES);
#endif

#ifdef EADDRINUSE
    ERRNO_CASE(EADDRINUSE);
#endif

#ifdef EADDRNOTAVAIL
    ERRNO_CASE(EADDRNOTAVAIL);
#endif

#ifdef EAFNOSUPPORT
    ERRNO_CASE(EAFNOSUPPORT);
#endif

#ifdef EAGAIN
    ERRNO_CASE(EAGAIN);
#endif

#ifdef EWOULDBLOCK
#if EAGAIN != EWOULDBLOCK
    ERRNO_CASE(EWOULDBLOCK);
#endif
#endif

#ifdef EALREADY
    ERRNO_CASE(EALREADY);
#endif

#ifdef EBADF
    ERRNO_CASE(EBADF);
#endif

#ifdef EBADMSG
    ERRNO_CASE(EBADMSG);
#endif

#ifdef EBUSY
    ERRNO_CASE(EBUSY);
#endif

#ifdef ECANCELED
    ERRNO_CASE(ECANCELED);
#endif

#ifdef ECHILD
    ERRNO_CASE(ECHILD);
#endif

#ifdef ECONNABORTED
    ERRNO_CASE(ECONNABORTED);
#endif

#ifdef ECONNREFUSED
    ERRNO_CASE(ECONNREFUSED);
#endif

#ifdef ECONNRESET
    ERRNO_CASE(ECONNRESET);
#endif

#ifdef EDEADLK
    ERRNO_CASE(EDEADLK);
#endif

#ifdef EDESTADDRREQ
    ERRNO_CASE(EDESTADDRREQ);
#endif

#ifdef EDOM
    ERRNO_CASE(EDOM);
#endif

#ifdef EDQUOT
    ERRNO_CASE(EDQUOT);
#endif

#ifdef EEXIST
    ERRNO_CASE(EEXIST);
#endif

#ifdef EFAULT
    ERRNO_CASE(EFAULT);
#endif

#ifdef EFBIG
    ERRNO_CASE(EFBIG);
#endif

#ifdef EHOSTUNREACH
    ERRNO_CASE(EHOSTUNREACH);
#endif

#ifdef EIDRM
    ERRNO_CASE(EIDRM);
#endif

#ifdef EILSEQ
    ERRNO_CASE(EILSEQ);
#endif

#ifdef EINPROGRESS
    ERRNO_CASE(EINPROGRESS);
#endif

#ifdef EINTR
    ERRNO_CASE(EINTR);
#endif

#ifdef EINVAL
    ERRNO_CASE(EINVAL);
#endif

#ifdef EIO
    ERRNO_CASE(EIO);
#endif

#ifdef EISCONN
    ERRNO_CASE(EISCONN);
#endif

#ifdef EISDIR
    ERRNO_CASE(EISDIR);
#endif

#ifdef ELOOP
    ERRNO_CASE(ELOOP);
#endif

#ifdef EMFILE
    ERRNO_CASE(EMFILE);
#endif

#ifdef EMLINK
    ERRNO_CASE(EMLINK);
#endif

#ifdef EMSGSIZE
    ERRNO_CASE(EMSGSIZE);
#endif

#ifdef EMULTIHOP
    ERRNO_CASE(EMULTIHOP);
#endif

#ifdef ENAMETOOLONG
    ERRNO_CASE(ENAMETOOLONG);
#endif

#ifdef ENETDOWN
    ERRNO_CASE(ENETDOWN);
#endif

#ifdef ENETRESET
    ERRNO_CASE(ENETRESET);
#endif

#ifdef ENETUNREACH
    ERRNO_CASE(ENETUNREACH);
#endif

#ifdef ENFILE
    ERRNO_CASE(ENFILE);
#endif

#ifdef ENOBUFS
    ERRNO_CASE(ENOBUFS);
#endif

#ifdef ENODATA
    ERRNO_CASE(ENODATA);
#endif

#ifdef ENODEV
    ERRNO_CASE(ENODEV);
#endif

#ifdef ENOENT
    ERRNO_CASE(ENOENT);
#endif

#ifdef ENOEXEC
    ERRNO_CASE(ENOEXEC);
#endif

#ifdef ENOLINK
    ERRNO_CASE(ENOLINK);
#endif

#ifdef ENOLCK
#if ENOLINK != ENOLCK
    ERRNO_CASE(ENOLCK);
#endif
#endif

#ifdef ENOMEM
    ERRNO_CASE(ENOMEM);
#endif

#ifdef ENOMSG
    ERRNO_CASE(ENOMSG);
#endif

#ifdef ENOPROTOOPT
    ERRNO_CASE(ENOPROTOOPT);
#endif

#ifdef ENOSPC
    ERRNO_CASE(ENOSPC);
#endif

#ifdef ENOSR
    ERRNO_CASE(ENOSR);
#endif

#ifdef ENOSTR
    ERRNO_CASE(ENOSTR);
#endif

#ifdef ENOSYS
    ERRNO_CASE(ENOSYS);
#endif

#ifdef ENOTCONN
    ERRNO_CASE(ENOTCONN);
#endif

#ifdef ENOTDIR
    ERRNO_CASE(ENOTDIR);
#endif

#ifdef ENOTEMPTY
#if ENOTEMPTY != EEXIST
    ERRNO_CASE(ENOTEMPTY);
#endif
#endif

#ifdef ENOTSOCK
    ERRNO_CASE(ENOTSOCK);
#endif

#ifdef ENOTSUP
    ERRNO_CASE(ENOTSUP);
#else
#ifdef EOPNOTSUPP
    ERRNO_CASE(EOPNOTSUPP);
#endif
#endif

#ifdef ENOTTY
    ERRNO_CASE(ENOTTY);
#endif

#ifdef ENXIO
    ERRNO_CASE(ENXIO);
#endif

#ifdef EOVERFLOW
    ERRNO_CASE(EOVERFLOW);
#endif

#ifdef EPERM
    ERRNO_CASE(EPERM);
#endif

#ifdef EPIPE
    ERRNO_CASE(EPIPE);
#endif

#ifdef EPROTO
    ERRNO_CASE(EPROTO);
#endif

#ifdef EPROTONOSUPPORT
    ERRNO_CASE(EPROTONOSUPPORT);
#endif

#ifdef EPROTOTYPE
    ERRNO_CASE(EPROTOTYPE);
#endif

#ifdef ERANGE
    ERRNO_CASE(ERANGE);
#endif

#ifdef EROFS
    ERRNO_CASE(EROFS);
#endif

#ifdef ESPIPE
    ERRNO_CASE(ESPIPE);
#endif

#ifdef ESRCH
    ERRNO_CASE(ESRCH);
#endif

#ifdef ESTALE
    ERRNO_CASE(ESTALE);
#endif

#ifdef ETIME
    ERRNO_CASE(ETIME);
#endif

#ifdef ETIMEDOUT
    ERRNO_CASE(ETIMEDOUT);
#endif

#ifdef ETXTBSY
    ERRNO_CASE(ETXTBSY);
#endif

#ifdef EXDEV
    ERRNO_CASE(EXDEV);
#endif

    default:
      return "";
  }
}

void PerIsolateMessageListener(Local<Message> message, Local<Value> error) {
  Isolate* isolate = message->GetIsolate();
  switch (message->ErrorLevel()) {
    case Isolate::MessageErrorLevel::kMessageWarning: {
      Environment* env = Environment::GetCurrent(isolate);
      if (!env) {
        break;
      }
      Utf8Value filename(isolate, message->GetScriptOrigin().ResourceName());
      // (filename):(line) (message)
      std::stringstream warning;
      warning << *filename;
      warning << ":";
      warning << message->GetLineNumber(env->context()).FromMaybe(-1);
      warning << " ";
      v8::String::Utf8Value msg(isolate, message->Get());
      warning << *msg;
      USE(ProcessEmitWarningGeneric(env, warning.str().c_str(), "V8"));
      break;
    }
    case Isolate::MessageErrorLevel::kMessageError:
      TriggerUncaughtException(isolate, error, message);
      break;
  }
}

void SetPrepareStackTraceCallback(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  CHECK(args[0]->IsFunction());
  env->set_prepare_stack_trace_callback(args[0].As<Function>());
}

static void SetEnhanceStackForFatalException(
    const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  CHECK(args[0]->IsFunction());
  CHECK(args[1]->IsFunction());
  env->set_enhance_fatal_stack_before_inspector(args[0].As<Function>());
  env->set_enhance_fatal_stack_after_inspector(args[1].As<Function>());
}

// Side effect-free stringification that will never throw exceptions.
static void NoSideEffectsToString(const FunctionCallbackInfo<Value>& args) {
  Local<Context> context = args.GetIsolate()->GetCurrentContext();
  Local<String> detail_string;
  if (args[0]->ToDetailString(context).ToLocal(&detail_string))
    args.GetReturnValue().Set(detail_string);
}

static void TriggerUncaughtException(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  Environment* env = Environment::GetCurrent(isolate);
  Local<Value> exception = args[0];
  Local<Message> message = Exception::CreateMessage(isolate, exception);
  if (env != nullptr && env->abort_on_uncaught_exception()) {
    ReportFatalException(
        env, exception, message, EnhanceFatalException::kEnhance);
    Abort();
  }
  bool from_promise = args[1]->IsTrue();
  errors::TriggerUncaughtException(isolate, exception, message, from_promise);
}

void Initialize(Local<Object> target,
                Local<Value> unused,
                Local<Context> context,
                void* priv) {
  Environment* env = Environment::GetCurrent(context);
  env->SetMethod(
      target, "setPrepareStackTraceCallback", SetPrepareStackTraceCallback);
  env->SetMethod(target,
                 "setEnhanceStackForFatalException",
                 SetEnhanceStackForFatalException);
  env->SetMethodNoSideEffect(
      target, "noSideEffectsToString", NoSideEffectsToString);
  env->SetMethod(target, "triggerUncaughtException", TriggerUncaughtException);
}

void DecorateErrorStack(Environment* env,
                        const errors::TryCatchScope& try_catch) {
  Local<Value> exception = try_catch.Exception();

  if (!exception->IsObject()) return;

  Local<Object> err_obj = exception.As<Object>();

  if (IsExceptionDecorated(env, err_obj)) return;

  AppendExceptionLine(env, exception, try_catch.Message(), CONTEXTIFY_ERROR);
  TryCatchScope try_catch_scope(env);  // Ignore exceptions below.
  MaybeLocal<Value> stack = err_obj->Get(env->context(), env->stack_string());
  MaybeLocal<Value> maybe_value =
      err_obj->GetPrivate(env->context(), env->arrow_message_private_symbol());

  Local<Value> arrow;
  if (!(maybe_value.ToLocal(&arrow) && arrow->IsString())) {
    return;
  }

  if (stack.IsEmpty() || !stack.ToLocalChecked()->IsString()) {
    return;
  }

  Local<String> decorated_stack = String::Concat(
      env->isolate(),
      String::Concat(env->isolate(),
                     arrow.As<String>(),
                     FIXED_ONE_BYTE_STRING(env->isolate(), "\n")),
      stack.ToLocalChecked().As<String>());
  USE(err_obj->Set(env->context(), env->stack_string(), decorated_stack));
  err_obj->SetPrivate(
      env->context(), env->decorated_private_symbol(), True(env->isolate()));
}

void TriggerUncaughtException(Isolate* isolate,
                              Local<Value> error,
                              Local<Message> message,
                              bool from_promise) {
  CHECK(!error.IsEmpty());
  HandleScope scope(isolate);

  if (message.IsEmpty()) message = Exception::CreateMessage(isolate, error);

  CHECK(isolate->InContext());
  Local<Context> context = isolate->GetCurrentContext();
  Environment* env = Environment::GetCurrent(context);
  if (env == nullptr) {
    // This means that the exception happens before Environment is assigned
    // to the context e.g. when there is a SyntaxError in a per-context
    // script - which usually indicates that there is a bug because no JS
    // error is supposed to be thrown at this point.
    // Since we don't have access to Environment here, there is not
    // much we can do, so we just print whatever is useful and crash.
    PrintException(isolate, context, error, message);
    Abort();
  }

  // Invoke process._fatalException() to give user a chance to handle it.
  // We have to grab it from the process object since this has been
  // monkey-patchable.
  Local<Object> process_object = env->process_object();
  Local<String> fatal_exception_string = env->fatal_exception_string();
  Local<Value> fatal_exception_function =
      process_object->Get(env->context(),
                          fatal_exception_string).ToLocalChecked();
  // If the exception happens before process._fatalException is attached
  // during bootstrap, or if the user has patched it incorrectly, exit
  // the current Node.js instance.
  if (!fatal_exception_function->IsFunction()) {
    ReportFatalException(
        env, error, message, EnhanceFatalException::kDontEnhance);
    env->Exit(6);
    return;
  }

  MaybeLocal<Value> handled;
  {
    // We do not expect the global uncaught exception itself to throw any more
    // exceptions. If it does, exit the current Node.js instance.
    errors::TryCatchScope try_catch(env,
                                    errors::TryCatchScope::CatchMode::kFatal);
    // Explicitly disable verbose exception reporting -
    // if process._fatalException() throws an error, we don't want it to
    // trigger the per-isolate message listener which will call this
    // function and recurse.
    try_catch.SetVerbose(false);
    Local<Value> argv[2] = { error,
                             Boolean::New(env->isolate(), from_promise) };

    handled = fatal_exception_function.As<Function>()->Call(
        env->context(), process_object, arraysize(argv), argv);
  }

  // If process._fatalException() throws, we are now exiting the Node.js
  // instance so return to continue the exit routine.
  // TODO(joyeecheung): return a Maybe here to prevent the caller from
  // stepping on the exit.
  if (handled.IsEmpty()) {
    return;
  }

  // The global uncaught exception handler returns true if the user handles it
  // by e.g. listening to `uncaughtException`. In that case, continue program
  // execution.
  // TODO(joyeecheung): This has been only checking that the return value is
  // exactly false. Investigate whether this can be turned to an "if true"
  // similar to how the worker global uncaught exception handler handles it.
  if (!handled.ToLocalChecked()->IsFalse()) {
    return;
  }

  // Now we are certain that the exception is fatal.
  ReportFatalException(env, error, message, EnhanceFatalException::kEnhance);
  RunAtExit(env);

  // If the global uncaught exception handler sets process.exitCode,
  // exit with that code. Otherwise, exit with 1.
  Local<String> exit_code = env->exit_code_string();
  Local<Value> code;
  if (process_object->Get(env->context(), exit_code).ToLocal(&code) &&
      code->IsInt32()) {
    env->Exit(code.As<Int32>()->Value());
  } else {
    env->Exit(1);
  }
}

void TriggerUncaughtException(Isolate* isolate, const v8::TryCatch& try_catch) {
  // If the try_catch is verbose, the per-isolate message listener is going to
  // handle it (which is going to call into another overload of
  // TriggerUncaughtException()).
  if (try_catch.IsVerbose()) {
    return;
  }

  // If the user calls TryCatch::TerminateExecution() on this TryCatch
  // they must call CancelTerminateExecution() again before invoking
  // TriggerUncaughtException() because it will invoke
  // process._fatalException() in the JS land.
  CHECK(!try_catch.HasTerminated());
  CHECK(try_catch.HasCaught());
  HandleScope scope(isolate);
  TriggerUncaughtException(isolate,
                           try_catch.Exception(),
                           try_catch.Message(),
                           false /* from_promise */);
}

}  // namespace errors

}  // namespace node

NODE_MODULE_CONTEXT_AWARE_INTERNAL(errors, node::errors::Initialize)
