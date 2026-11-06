#pragma once

#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace rq {

class PythonEngine;
struct JobResult;

// Shows whether the embedded Python is working, and demonstrates calling it.
class HealthPanel : public QWidget {
  Q_OBJECT

 public:
  explicit HealthPanel(PythonEngine& engine, QWidget* parent = nullptr);

 signals:
  void pythonStateChanged(const QString& state);

 private:
  void runHealthCheck();
  void onStarted();
  void onStartFailed(const QString& reason);
  void onJobFinished(const JobResult& result);
  void showHealth(const JobResult& result);

  PythonEngine& engine_;
  quint64 healthJob_ = 0;

  QPushButton* healthButton_;
  QLabel* versionValue_;
  QLabel* prefixValue_;
  QLabel* isolationValue_;
  QLabel* numpyValue_;
  QLabel* healthError_;
  QPlainTextEdit* healthDetails_;
};

}  // namespace rq
