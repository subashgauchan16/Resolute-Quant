// The only file that includes Python. It owns the embedded interpreter.
#include <pybind11/embed.h>

#include <exception>
#include <string>

#include "bridge/PythonHost.h"

namespace py = pybind11;

namespace {

struct RunningJob {
  RqProgressFn onProgress;
  RqCancelledFn isCancelled;
  void* context;
};

RunningJob* g_job = nullptr;           // Only touched while holding the GIL.
PyThreadState* g_idleThread = nullptr;  // Set while idle, when the GIL is released.

std::wstring join(const std::wstring& base, const wchar_t* child) { return base + L"\\" + child; }

// Fills the isolated config. It ignores PYTHONHOME, PYTHONPATH, user packages and the registry.
PyStatus configure(PyConfig& config, const std::wstring& runtime, const std::wstring& engine) {
  PyConfig_InitIsolatedConfig(&config);
  PyStatus status = PyConfig_SetString(&config, &config.home, runtime.c_str());
  if (PyStatus_Exception(status)) return status;
  const std::wstring program = join(runtime, L"python.exe");
  status = PyConfig_SetString(&config, &config.program_name, program.c_str());
  if (PyStatus_Exception(status)) return status;

  config.module_search_paths_set = 1;
  const std::wstring paths[] = {engine, join(runtime, L"Lib"), join(runtime, L"DLLs"),
                                join(runtime, L"Lib\\site-packages")};
  for (const std::wstring& path : paths) {
    status = PyWideStringList_Append(&config.module_search_paths, path.c_str());
    if (PyStatus_Exception(status)) return status;
  }
  return PyStatus_Ok();
}

std::string describeValue(const py::error_already_set& error) {
  try {
    return std::string(py::str(error.value()));
  } catch (...) {
    return "The exception could not be converted to text";
  }
}

std::string describeTraceback(const py::error_already_set& error) {
  try {
    py::object lines =
        py::module_::import("traceback").attr("format_exception")(error.type(), error.value(), error.trace());
    return std::string(py::str(py::str("").attr("join")(lines)));
  } catch (...) {
    return error.what();
  }
}

}  // namespace

PYBIND11_EMBEDDED_MODULE(_rq_host, module) {
  module.def("report_progress", [](double fraction, const std::string& message) {
    if (g_job != nullptr) g_job->onProgress(g_job->context, fraction, message.c_str());
  });
  module.def("is_cancelled", [] { return g_job != nullptr && g_job->isCancelled(g_job->context) != 0; });
}

RQ_HOST_API int rq_py_start(const wchar_t* runtimeDir, const wchar_t* engineDir, RqErrorFn onError,
                            void* context) {
  PyConfig config;
  const PyStatus status = configure(config, runtimeDir, engineDir);
  if (PyStatus_Exception(status)) {
    PyConfig_Clear(&config);
    onError(context, status.err_msg != nullptr ? status.err_msg : "Python could not be configured");
    return 0;
  }
  try {
    // Signal handlers stay with Qt: the isolated config does not install Python's.
    py::initialize_interpreter(&config, 0, nullptr, false);
  } catch (const std::exception& error) {
    PyConfig_Clear(&config);
    onError(context, error.what());
    return 0;
  }
  PyConfig_Clear(&config);
  g_idleThread = PyEval_SaveThread();
  return 1;
}

RQ_HOST_API void rq_py_run(const char* job, const char* argsJson, RqProgressFn onProgress,
                           RqCancelledFn isCancelled, RqResultFn onResult, void* context) {
  PyEval_RestoreThread(g_idleThread);
  {
    RunningJob running{onProgress, isCancelled, context};
    g_job = &running;
    try {
      py::object result = py::module_::import("rq_engine.runner").attr("run")(job, argsJson);
      const std::string payload = py::str(result);
      onResult(context, RQ_JOB_OK, payload.c_str(), "", "", "");
    } catch (py::error_already_set& error) {
      const std::string type = py::str(error.type().attr("__name__"));
      const std::string message = describeValue(error);
      const std::string traceback = describeTraceback(error);
      const int status = type == "JobCancelled" ? RQ_JOB_CANCELLED : RQ_JOB_ERROR;
      onResult(context, status, "", type.c_str(), message.c_str(), traceback.c_str());
    } catch (const std::exception& error) {
      onResult(context, RQ_JOB_ERROR, "", "BridgeError", error.what(), "");
    }
    g_job = nullptr;
  }
  g_idleThread = PyEval_SaveThread();
}

RQ_HOST_API void rq_py_stop() {
  PyEval_RestoreThread(g_idleThread);
  g_idleThread = nullptr;
  py::finalize_interpreter();
}
