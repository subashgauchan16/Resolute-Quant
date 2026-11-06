#include "bridge/PythonEngine.h"

#include <windows.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QThread>
#include <QtDebug>
#include <algorithm>
#include <string>

#include "bridge/PythonHost.h"

namespace rq {

namespace {

struct Host {
  HMODULE module = nullptr;
  RqRunFn run = nullptr;
  RqStopFn stop = nullptr;
};

struct RunContext {
  PythonEngine* engine;
  JobResult* result;
};

std::wstring nativePath(const QString& path) {
  return QDir::toNativeSeparators(path).toStdWString();
}

QString lastWindowsError() {
  wchar_t* text = nullptr;
  const DWORD code = GetLastError();
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                 nullptr, code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
  const QString message = text != nullptr ? QString::fromWCharArray(text).trimmed() : QString::number(code);
  LocalFree(text);
  return message;
}

// Checks the runtime and loads the bridge. Returns an empty string on success, or the reason.
QString loadHost(const PythonEngine::Paths& paths, Host& host) {
  const QDir runtime(paths.runtimeDir);
  if (!runtime.exists()) {
    return QStringLiteral("The private Python runtime was not found at %1. Run native\\tools\\setup_runtime.ps1.")
        .arg(QDir::toNativeSeparators(paths.runtimeDir));
  }
  for (const char* required : {"python313.dll", "Lib/encodings/__init__.py"}) {
    if (!QFileInfo::exists(runtime.filePath(QString::fromLatin1(required)))) {
      return QStringLiteral("The private Python runtime at %1 is incomplete: %2 is missing.")
          .arg(QDir::toNativeSeparators(paths.runtimeDir), QString::fromLatin1(required));
    }
  }
  if (!QFileInfo::exists(QDir(paths.engineDir).filePath(QStringLiteral("rq_engine/runner.py")))) {
    return QStringLiteral("The rq_engine package was not found in %1.")
        .arg(QDir::toNativeSeparators(paths.engineDir));
  }

  // Report load failures here, rather than as a Windows dialog.
  DWORD previousMode = 0;
  SetThreadErrorMode(SEM_FAILCRITICALERRORS, &previousMode);
  const std::wstring pythonDll = nativePath(runtime.filePath(QStringLiteral("python313.dll")));
  const std::wstring hostDll = nativePath(paths.hostDll);
  const HMODULE python = LoadLibraryExW(pythonDll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  QString failure;
  if (python == nullptr) {
    failure = QStringLiteral("python313.dll could not be loaded: %1").arg(lastWindowsError());
  } else {
    host.module = LoadLibraryExW(hostDll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (host.module == nullptr) {
      failure = QStringLiteral("%1 could not be loaded: %2").arg(paths.hostDll, lastWindowsError());
    }
  }
  SetThreadErrorMode(previousMode, nullptr);
  return failure;
}

void onStartError(void* context, const char* message) {
  *static_cast<QString*>(context) = QString::fromUtf8(message);
}

void onProgress(void* context, double fraction, const char* message) {
  auto* run = static_cast<RunContext*>(context);
  emit run->engine->progress(run->result->id, std::clamp(fraction, 0.0, 1.0), QString::fromUtf8(message));
}

int isCancelled(void* context) {
  auto* run = static_cast<RunContext*>(context);
  return run->engine->isCancelRequested(run->result->id) ? 1 : 0;
}

void onResult(void* context, int status, const char* payload, const char* errorType, const char* message,
              const char* traceback) {
  JobResult& result = *static_cast<RunContext*>(context)->result;
  if (status == RQ_JOB_OK) {
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(QByteArray(payload), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
      result.status = JobStatus::Error;
      result.errorType = QStringLiteral("BridgeError");
      result.message = QStringLiteral("The job did not return a JSON object.");
      return;
    }
    result.status = JobStatus::Ok;
    result.value = document.object();
    return;
  }
  result.status = status == RQ_JOB_CANCELLED ? JobStatus::Cancelled : JobStatus::Error;
  result.errorType = QString::fromUtf8(errorType);
  result.message = QString::fromUtf8(message);
  result.traceback = QString::fromUtf8(traceback);
}

}  // namespace

PythonEngine::Paths PythonEngine::locate(const QString& runtimeOverride) {
  const QDir exeDir(QCoreApplication::applicationDirPath());
  Paths paths;
  paths.engineDir = exeDir.filePath(QStringLiteral("python"));
  paths.hostDll = exeDir.filePath(QStringLiteral("rq_python_host.dll"));
  paths.runtimeDir = exeDir.filePath(QStringLiteral("runtime/python"));

  if (!runtimeOverride.isEmpty()) {
    paths.runtimeDir = runtimeOverride;
  } else if (!QDir(paths.runtimeDir).exists()) {
    QFile devPath(exeDir.filePath(QStringLiteral("dev_runtime_path.txt")));
    if (devPath.open(QIODevice::ReadOnly | QIODevice::Text)) {
      paths.runtimeDir = QString::fromUtf8(devPath.readAll()).trimmed();
    }
  }
  return paths;
}

PythonEngine::PythonEngine(Paths paths, QObject* parent) : QObject(parent), paths_(std::move(paths)) {
  qRegisterMetaType<rq::JobResult>();
}

PythonEngine::~PythonEngine() { shutdownOrTerminate(); }

void PythonEngine::start() {
  if (thread_ != nullptr) return;
  thread_ = QThread::create([this] { threadMain(); });
  thread_->setObjectName(QStringLiteral("python"));
  thread_->start();
}

quint64 PythonEngine::submit(const QString& job, const QJsonObject& args) {
  std::lock_guard lock(mutex_);
  if (stopping_) return 0;
  const quint64 id = nextId_++;
  queue_.push_back({id, job, QJsonDocument(args).toJson(QJsonDocument::Compact)});
  wake_.notify_one();
  return id;
}

void PythonEngine::cancel(quint64 id) {
  bool removed = false;
  QString name;
  {
    std::lock_guard lock(mutex_);
    const auto queued = std::find_if(queue_.begin(), queue_.end(), [id](const Job& job) { return job.id == id; });
    if (queued != queue_.end()) {
      name = queued->name;
      queue_.erase(queued);
      removed = true;
    }
  }
  if (removed) {
    JobResult result;
    result.id = id;
    result.job = name;
    result.status = JobStatus::Cancelled;
    result.message = QStringLiteral("Cancelled");
    QMetaObject::invokeMethod(this, [this, result] { emit jobFinished(result); }, Qt::QueuedConnection);
    return;
  }
  cancelId_ = id;
}

bool PythonEngine::isCancelRequested(quint64 id) const {
  return stopRequested_.load() || cancelId_.load() == id;
}

bool PythonEngine::shutdown(int graceMs) {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
    queue_.clear();
  }
  stopRequested_ = true;
  wake_.notify_all();
  if (thread_ == nullptr) return true;
  if (!thread_->wait(graceMs)) return false;
  delete thread_;
  thread_ = nullptr;
  return true;
}

void PythonEngine::shutdownOrTerminate(int graceMs) {
  if (shutdown(graceMs)) return;
  qCritical("Python did not stop within %d ms, probably stuck in a native call. Ending the process.", graceMs);
  TerminateProcess(GetCurrentProcess(), kExitForcedShutdown);
}

void PythonEngine::threadMain() {
  Host host;
  QString failure = loadHost(paths_, host);
  bool running = false;
  if (failure.isEmpty()) {
    const auto startFn = reinterpret_cast<RqStartFn>(GetProcAddress(host.module, "rq_py_start"));
    host.run = reinterpret_cast<RqRunFn>(GetProcAddress(host.module, "rq_py_run"));
    host.stop = reinterpret_cast<RqStopFn>(GetProcAddress(host.module, "rq_py_stop"));
    if (startFn == nullptr || host.run == nullptr || host.stop == nullptr) {
      failure = QStringLiteral("rq_python_host.dll does not match this version of the app.");
    } else {
      const std::wstring runtime = nativePath(paths_.runtimeDir);
      const std::wstring engine = nativePath(paths_.engineDir);
      running = startFn(runtime.c_str(), engine.c_str(), onStartError, &failure) != 0;
      if (!running && failure.isEmpty()) failure = QStringLiteral("Python failed to start.");
    }
  }

  if (running) {
    emit started();
  } else {
    qCritical("Python failed to start: %s", qUtf8Printable(failure));
    emit startFailed(failure);
  }

  for (;;) {
    Job job;
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (stopping_) break;
      job = std::move(queue_.front());
      queue_.pop_front();
    }

    JobResult result;
    result.id = job.id;
    result.job = job.name;
    if (running) {
      runningId_ = job.id;
      RunContext context{this, &result};
      host.run(job.name.toUtf8().constData(), job.argsJson.constData(), onProgress, isCancelled, onResult, &context);
      runningId_ = 0;
    } else {
      result.errorType = QStringLiteral("PythonUnavailable");
      result.message = failure;
    }
    if (result.status == JobStatus::Error) {
      qWarning("Job %s failed: %s: %s", qUtf8Printable(result.job), qUtf8Printable(result.errorType),
               qUtf8Printable(result.message));
    }
    emit jobFinished(result);
  }

  if (running) host.stop();
}

}  // namespace rq
