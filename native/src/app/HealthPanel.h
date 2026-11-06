#pragma once

#include <QElapsedTimer>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTimer;

namespace rq {

class PythonEngine;
struct JobResult;

// Shows whether the embedded Python is working, and demonstrates calling it.
class HealthPanel : public QWidget {
  Q_OBJECT

 public:
  explicit HealthPanel(PythonEngine& engine, QWidget* parent = nullptr);

  // The longest gap between window updates during the last long job, in milliseconds.
  qint64 longJobMaxGapMs() const { return maxGapMs_; }

 signals:
  void pythonStateChanged(const QString& state);

 private:
  QWidget* buildHealthGroup();
  QWidget* buildCalculationGroup();
  QWidget* buildLongJobGroup();

  void runHealthCheck();
  void runCalculation();
  void startLongJob();
  void cancelLongJob();
  void onStarted();
  void onStartFailed(const QString& reason);
  void onProgress(quint64 id, double fraction, const QString& message);
  void onJobFinished(const JobResult& result);
  void showHealth(const JobResult& result);
  void showCalculation(const JobResult& result);
  void showLongJob(const JobResult& result);
  void onHeartbeat();

  PythonEngine& engine_;
  bool pythonRunning_ = false;
  quint64 healthJob_ = 0;
  quint64 calculationJob_ = 0;
  quint64 longJob_ = 0;

  QPushButton* healthButton_;
  QLabel* versionValue_;
  QLabel* prefixValue_;
  QLabel* isolationValue_;
  QLabel* numpyValue_;
  QLabel* healthError_;
  QPlainTextEdit* healthDetails_;

  QLineEdit* calculationInput_;
  QPushButton* calculationButton_;
  QLabel* calculationResult_;
  QLabel* calculationError_;
  QPlainTextEdit* calculationDetails_;

  QPushButton* longStartButton_;
  QPushButton* longCancelButton_;
  QProgressBar* longProgress_;
  QLabel* longStatus_;
  QLabel* heartbeatLabel_;

  QTimer* heartbeatTimer_;
  QElapsedTimer heartbeatClock_;
  quint64 heartbeatCount_ = 0;
  qint64 maxGapMs_ = 0;
};

}  // namespace rq
