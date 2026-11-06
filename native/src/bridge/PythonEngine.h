#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>

#include "bridge/PythonResult.h"

class QThread;

namespace rq {

// Owns Python on one background thread and runs one job at a time from a queue. The window calls
// these methods from the UI thread and never waits on Python, except briefly in shutdown. Results
// arrive through signals, which Qt delivers on the UI thread.
class PythonEngine : public QObject {
  Q_OBJECT

 public:
  struct Paths {
    QString runtimeDir;  // Holds python313.dll and Lib.
    QString engineDir;   // Holds the rq_engine package.
    QString hostDll;     // rq_python_host.dll.
  };

  static constexpr int kExitForcedShutdown = 4;
  static constexpr int kShutdownGraceMs = 3000;

  // A non-empty runtimeOverride wins. Otherwise, a runtime beside the exe, then the development
  // runtime named by dev_runtime_path.txt, which only exists in the build folder.
  static Paths locate(const QString& runtimeOverride = {});

  explicit PythonEngine(Paths paths, QObject* parent = nullptr);
  ~PythonEngine() override;

  void start();
  quint64 submit(const QString& job, const QJsonObject& args = {});  // Returns 0 after shutdown.
  void cancel(quint64 id);

  // Refuses new jobs, signals cancel and waits. False means the Python thread did not stop in time.
  bool shutdown(int graceMs = kShutdownGraceMs);
  // shutdown(), but if Python is stuck in a native call, logs it and ends the process: waiting
  // forever would be worse, and it is not safe to destroy a running thread.
  void shutdownOrTerminate(int graceMs = kShutdownGraceMs);

  const Paths& paths() const { return paths_; }
  bool isCancelRequested(quint64 id) const;

 signals:
  void started();
  void startFailed(const QString& reason);
  void progress(quint64 id, double fraction, const QString& message);
  void jobFinished(const rq::JobResult& result);

 private:
  struct Job {
    quint64 id;
    QString name;
    QByteArray argsJson;
  };

  void threadMain();

  Paths paths_;
  QThread* thread_ = nullptr;

  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Job> queue_;
  quint64 nextId_ = 1;
  bool stopping_ = false;

  std::atomic<quint64> runningId_{0};
  std::atomic<quint64> cancelId_{0};
  std::atomic<bool> stopRequested_{false};
};

}  // namespace rq
