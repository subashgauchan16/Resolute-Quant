#pragma once

// The boundary between the app and Python. The app loads rq_python_host.dll at runtime, after
// checking the private Python exists, so a missing runtime gives a message rather than a Windows
// loader error. Only plain C types cross here. Every function must be called on the same thread.

#ifdef RQ_PYTHON_HOST_BUILD
#define RQ_HOST_API extern "C" __declspec(dllexport)
#else
#define RQ_HOST_API extern "C" __declspec(dllimport)
#endif

enum RqJobStatus { RQ_JOB_OK = 0, RQ_JOB_ERROR = 1, RQ_JOB_CANCELLED = 2 };

using RqProgressFn = void (*)(void* context, double fraction, const char* message);
using RqCancelledFn = int (*)(void* context);
// For RQ_JOB_OK, payload is the job's result as JSON. Otherwise, the other three describe the error.
using RqResultFn = void (*)(void* context, int status, const char* payload, const char* errorType,
                            const char* message, const char* traceback);
using RqErrorFn = void (*)(void* context, const char* message);
// Called on Python's own background threads, for messages that are not the result of a job.
using RqEventFn = void (*)(void* context, const char* json);
using RqSetEventHandlerFn = void (*)(RqEventFn onEvent, void* context);

using RqStartFn = int (*)(const wchar_t* runtimeDir, const wchar_t* engineDir, RqErrorFn onError,
                          void* context);
using RqRunFn = void (*)(const char* job, const char* argsJson, RqProgressFn onProgress,
                         RqCancelledFn isCancelled, RqResultFn onResult, void* context);
using RqStopFn = void (*)();

#ifdef RQ_PYTHON_HOST_BUILD
RQ_HOST_API int rq_py_start(const wchar_t* runtimeDir, const wchar_t* engineDir, RqErrorFn onError,
                            void* context);
RQ_HOST_API void rq_py_run(const char* job, const char* argsJson, RqProgressFn onProgress,
                           RqCancelledFn isCancelled, RqResultFn onResult, void* context);
RQ_HOST_API void rq_py_stop();
RQ_HOST_API void rq_py_set_event_handler(RqEventFn onEvent, void* context);
#endif
