#include "app/HealthPanel.h"

#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

#include "bridge/PythonEngine.h"

namespace rq {

namespace {

constexpr int kHeartbeatMs = 50;

QLabel* makeValue(const QString& objectName, const QString& text = QStringLiteral("Not checked")) {
  auto* label = new QLabel(text);
  label->setObjectName(objectName);
  label->setWordWrap(true);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  return label;
}

QLabel* makeError(const QString& objectName) {
  auto* label = new QLabel;
  label->setObjectName(objectName);
  label->setStyleSheet(QStringLiteral("color: #ef9a8f;"));
  label->setWordWrap(true);
  label->hide();
  return label;
}

QPlainTextEdit* makeDetails(const QString& objectName) {
  auto* details = new QPlainTextEdit;
  details->setObjectName(objectName);
  details->setReadOnly(true);
  details->setMaximumHeight(110);
  details->hide();
  return details;
}

QPushButton* makeButton(const QString& objectName, const QString& text) {
  auto* button = new QPushButton(text);
  button->setObjectName(objectName);
  button->setEnabled(false);
  return button;
}

}  // namespace

HealthPanel::HealthPanel(PythonEngine& engine, QWidget* parent)
    : QWidget(parent),
      engine_(engine),
      healthButton_(makeButton(QStringLiteral("runHealthCheck"), QStringLiteral("Run health check"))),
      versionValue_(makeValue(QStringLiteral("pythonVersion"))),
      prefixValue_(makeValue(QStringLiteral("pythonPrefix"))),
      isolationValue_(makeValue(QStringLiteral("pythonIsolation"))),
      numpyValue_(makeValue(QStringLiteral("numpyVersion"))),
      healthError_(makeError(QStringLiteral("healthError"))),
      healthDetails_(makeDetails(QStringLiteral("healthDetails"))),
      calculationInput_(new QLineEdit(QStringLiteral("2, 4, 4, 4, 5, 5, 7, 9"))),
      calculationButton_(makeButton(QStringLiteral("calculate"), QStringLiteral("Calculate in Python"))),
      calculationResult_(makeValue(QStringLiteral("calculationResult"), QString())),
      calculationError_(makeError(QStringLiteral("calculationError"))),
      calculationDetails_(makeDetails(QStringLiteral("calculationDetails"))),
      longStartButton_(makeButton(QStringLiteral("longStart"), QStringLiteral("Start 5-second job"))),
      longCancelButton_(makeButton(QStringLiteral("longCancel"), QStringLiteral("Cancel"))),
      longProgress_(new QProgressBar),
      longStatus_(makeValue(QStringLiteral("longStatus"), QStringLiteral("Not started"))),
      heartbeatLabel_(makeValue(QStringLiteral("heartbeat"), QString())),
      heartbeatTimer_(new QTimer(this)) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(24, 20, 24, 20);

  auto* title = new QLabel(QStringLiteral("Python health"));
  title->setObjectName(QStringLiteral("pageTitle"));
  layout->addWidget(title);
  layout->addWidget(buildHealthGroup());
  layout->addWidget(buildCalculationGroup());
  layout->addWidget(buildLongJobGroup());
  layout->addStretch();

  connect(&engine_, &PythonEngine::started, this, &HealthPanel::onStarted);
  connect(&engine_, &PythonEngine::startFailed, this, &HealthPanel::onStartFailed);
  connect(&engine_, &PythonEngine::progress, this, &HealthPanel::onProgress);
  connect(&engine_, &PythonEngine::jobFinished, this, &HealthPanel::onJobFinished);

  connect(heartbeatTimer_, &QTimer::timeout, this, &HealthPanel::onHeartbeat);
  heartbeatClock_.start();
  heartbeatTimer_->start(kHeartbeatMs);
}

QWidget* HealthPanel::buildHealthGroup() {
  auto* group = new QGroupBox(QStringLiteral("Embedded interpreter"));
  auto* form = new QFormLayout;
  form->addRow(QStringLiteral("Python"), versionValue_);
  form->addRow(QStringLiteral("Location"), prefixValue_);
  form->addRow(QStringLiteral("Isolation"), isolationValue_);
  form->addRow(QStringLiteral("NumPy"), numpyValue_);

  auto* layout = new QVBoxLayout(group);
  layout->addLayout(form);
  layout->addWidget(healthError_);
  layout->addWidget(healthDetails_);
  layout->addWidget(healthButton_, 0, Qt::AlignLeft);
  connect(healthButton_, &QPushButton::clicked, this, &HealthPanel::runHealthCheck);
  return group;
}

QWidget* HealthPanel::buildCalculationGroup() {
  auto* group = new QGroupBox(QStringLiteral("Calculation through Python"));
  calculationInput_->setObjectName(QStringLiteral("calculationInput"));
  calculationInput_->setPlaceholderText(QStringLiteral("Numbers separated by commas or spaces"));

  auto* row = new QHBoxLayout;
  row->addWidget(calculationInput_, 1);
  row->addWidget(calculationButton_);

  auto* note = new QLabel(QStringLiteral("NumPy works out the mean and sample standard deviation of your numbers. "
                                         "Try a word in the list to see how a Python error appears."));
  note->setObjectName(QStringLiteral("muted"));
  note->setWordWrap(true);

  auto* layout = new QVBoxLayout(group);
  layout->addWidget(note);
  layout->addLayout(row);
  layout->addWidget(calculationResult_);
  layout->addWidget(calculationError_);
  layout->addWidget(calculationDetails_);

  connect(calculationButton_, &QPushButton::clicked, this, &HealthPanel::runCalculation);
  connect(calculationInput_, &QLineEdit::returnPressed, this, &HealthPanel::runCalculation);
  return group;
}

QWidget* HealthPanel::buildLongJobGroup() {
  auto* group = new QGroupBox(QStringLiteral("Long-running job"));
  auto* note = new QLabel(QStringLiteral("Python keeps the processor busy while the window stays live. "
                                         "The job does no useful work."));
  note->setObjectName(QStringLiteral("muted"));
  note->setWordWrap(true);

  longProgress_->setObjectName(QStringLiteral("longProgress"));
  longProgress_->setRange(0, 100);
  longProgress_->setValue(0);

  auto* buttons = new QHBoxLayout;
  buttons->addWidget(longStartButton_);
  buttons->addWidget(longCancelButton_);
  buttons->addStretch();

  auto* layout = new QVBoxLayout(group);
  layout->addWidget(note);
  layout->addWidget(longProgress_);
  layout->addLayout(buttons);
  layout->addWidget(longStatus_);
  layout->addWidget(heartbeatLabel_);

  connect(longStartButton_, &QPushButton::clicked, this, &HealthPanel::startLongJob);
  connect(longCancelButton_, &QPushButton::clicked, this, &HealthPanel::cancelLongJob);
  return group;
}

void HealthPanel::runHealthCheck() {
  healthButton_->setEnabled(false);
  healthJob_ = engine_.submit(QStringLiteral("health"));
}

void HealthPanel::runCalculation() {
  if (!pythonRunning_ || calculationJob_ != 0) return;
  calculationButton_->setEnabled(false);
  calculationJob_ = engine_.submit(QStringLiteral("summary_statistics"),
                                   QJsonObject{{QStringLiteral("text"), calculationInput_->text()}});
}

void HealthPanel::startLongJob() {
  maxGapMs_ = 0;
  longProgress_->setValue(0);
  longStatus_->setText(QStringLiteral("Running"));
  longStartButton_->setEnabled(false);
  longCancelButton_->setEnabled(true);
  longJob_ = engine_.submit(QStringLiteral("long_demo"), QJsonObject{{QStringLiteral("seconds"), 5.0}});
}

void HealthPanel::cancelLongJob() {
  if (longJob_ == 0) return;
  longCancelButton_->setEnabled(false);
  longStatus_->setText(QStringLiteral("Cancelling"));
  engine_.cancel(longJob_);
}

void HealthPanel::onStarted() {
  pythonRunning_ = true;
  emit pythonStateChanged(QStringLiteral("Python: running"));
  calculationButton_->setEnabled(true);
  longStartButton_->setEnabled(true);
  runHealthCheck();
}

void HealthPanel::onStartFailed(const QString& reason) {
  emit pythonStateChanged(QStringLiteral("Python: unavailable"));
  const QString unavailable = QStringLiteral("Unavailable");
  versionValue_->setText(unavailable);
  prefixValue_->setText(unavailable);
  isolationValue_->setText(unavailable);
  numpyValue_->setText(unavailable);
  healthError_->setText(reason);
  healthError_->show();
}

void HealthPanel::onProgress(quint64 id, double fraction, const QString& message) {
  if (id != longJob_) return;
  longProgress_->setValue(static_cast<int>(fraction * 100.0));
  longStatus_->setText(message);
}

void HealthPanel::onJobFinished(const JobResult& result) {
  if (result.id == healthJob_) {
    showHealth(result);
  } else if (result.id == calculationJob_) {
    showCalculation(result);
  } else if (result.id == longJob_) {
    showLongJob(result);
  }
}

void HealthPanel::showHealth(const JobResult& result) {
  healthJob_ = 0;
  healthButton_->setEnabled(true);
  const bool ok = result.status == JobStatus::Ok;
  healthError_->setVisible(!ok);
  healthDetails_->setVisible(!ok && !result.traceback.isEmpty());
  if (!ok) {
    healthError_->setText(QStringLiteral("%1: %2").arg(result.errorType, result.message));
    healthDetails_->setPlainText(result.traceback);
    return;
  }

  const QJsonObject& value = result.value;
  versionValue_->setText(value[QStringLiteral("python_version")].toString());
  prefixValue_->setText(value[QStringLiteral("prefix")].toString());
  const bool isolated = value[QStringLiteral("isolated")].toBool() &&
                        value[QStringLiteral("ignores_environment")].toBool() &&
                        !value[QStringLiteral("user_site_enabled")].toBool();
  isolationValue_->setText(isolated ? QStringLiteral("Active: system Python settings and user packages are ignored")
                                    : QStringLiteral("Not active"));
  const QString numpy = value[QStringLiteral("numpy_version")].toString();
  numpyValue_->setText(numpy.isEmpty()
                           ? QStringLiteral("Missing: %1").arg(value[QStringLiteral("numpy_error")].toString())
                           : numpy);
}

void HealthPanel::showCalculation(const JobResult& result) {
  calculationJob_ = 0;
  calculationButton_->setEnabled(true);
  const bool ok = result.status == JobStatus::Ok;
  calculationResult_->setVisible(ok);
  calculationError_->setVisible(!ok);
  calculationDetails_->setVisible(!ok && !result.traceback.isEmpty());
  if (!ok) {
    calculationError_->setText(QStringLiteral("%1: %2").arg(result.errorType, result.message));
    calculationDetails_->setPlainText(result.traceback);
    return;
  }
  const QJsonObject& value = result.value;
  calculationResult_->setText(QStringLiteral("Count %1, mean %2, sample standard deviation %3")
                                  .arg(value[QStringLiteral("count")].toInt())
                                  .arg(value[QStringLiteral("mean")].toDouble(), 0, 'g', 12)
                                  .arg(value[QStringLiteral("sample_std_dev")].toDouble(), 0, 'g', 12));
}

void HealthPanel::showLongJob(const JobResult& result) {
  longJob_ = 0;
  longStartButton_->setEnabled(pythonRunning_);
  longCancelButton_->setEnabled(false);
  QString outcome;
  switch (result.status) {
    case JobStatus::Ok:
      longProgress_->setValue(100);
      outcome = QStringLiteral("Finished");
      break;
    case JobStatus::Cancelled:
      outcome = QStringLiteral("Cancelled");
      break;
    case JobStatus::Error:
      outcome = QStringLiteral("%1: %2").arg(result.errorType, result.message);
      break;
  }
  longStatus_->setText(QStringLiteral("%1. Longest gap between window updates: %2 ms, where about %3 ms is normal.")
                           .arg(outcome)
                           .arg(maxGapMs_)
                           .arg(kHeartbeatMs));
}

void HealthPanel::onHeartbeat() {
  const qint64 gap = heartbeatClock_.restart();
  if (longJob_ != 0) maxGapMs_ = std::max(maxGapMs_, gap);
  ++heartbeatCount_;
  heartbeatLabel_->setText(QStringLiteral("Window heartbeat: %1").arg(heartbeatCount_));
}

}  // namespace rq
