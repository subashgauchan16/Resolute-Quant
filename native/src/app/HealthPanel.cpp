#include "app/HealthPanel.h"

#include <QFormLayout>
#include <QGroupBox>
#include <QJsonObject>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "bridge/PythonEngine.h"

namespace rq {

namespace {

QLabel* makeValue(const QString& objectName) {
  auto* label = new QLabel(QStringLiteral("Not checked"));
  label->setObjectName(objectName);
  label->setWordWrap(true);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  return label;
}

}  // namespace

HealthPanel::HealthPanel(PythonEngine& engine, QWidget* parent)
    : QWidget(parent),
      engine_(engine),
      healthButton_(new QPushButton(QStringLiteral("Run health check"))),
      versionValue_(makeValue(QStringLiteral("pythonVersion"))),
      prefixValue_(makeValue(QStringLiteral("pythonPrefix"))),
      isolationValue_(makeValue(QStringLiteral("pythonIsolation"))),
      numpyValue_(makeValue(QStringLiteral("numpyVersion"))),
      healthError_(new QLabel),
      healthDetails_(new QPlainTextEdit) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(24, 20, 24, 20);

  auto* title = new QLabel(QStringLiteral("Python health"));
  title->setObjectName(QStringLiteral("pageTitle"));
  layout->addWidget(title);

  auto* healthBox = new QGroupBox(QStringLiteral("Embedded interpreter"));
  auto* form = new QFormLayout;
  form->addRow(QStringLiteral("Python"), versionValue_);
  form->addRow(QStringLiteral("Location"), prefixValue_);
  form->addRow(QStringLiteral("Isolation"), isolationValue_);
  form->addRow(QStringLiteral("NumPy"), numpyValue_);

  healthError_->setObjectName(QStringLiteral("errorText"));
  healthError_->setWordWrap(true);
  healthError_->hide();
  healthDetails_->setObjectName(QStringLiteral("healthDetails"));
  healthDetails_->setReadOnly(true);
  healthDetails_->setMaximumHeight(120);
  healthDetails_->hide();
  healthButton_->setObjectName(QStringLiteral("runHealthCheck"));
  healthButton_->setEnabled(false);

  auto* healthLayout = new QVBoxLayout(healthBox);
  healthLayout->addLayout(form);
  healthLayout->addWidget(healthError_);
  healthLayout->addWidget(healthDetails_);
  healthLayout->addWidget(healthButton_, 0, Qt::AlignLeft);
  layout->addWidget(healthBox);
  layout->addStretch();

  connect(healthButton_, &QPushButton::clicked, this, &HealthPanel::runHealthCheck);
  connect(&engine_, &PythonEngine::started, this, &HealthPanel::onStarted);
  connect(&engine_, &PythonEngine::startFailed, this, &HealthPanel::onStartFailed);
  connect(&engine_, &PythonEngine::jobFinished, this, &HealthPanel::onJobFinished);
}

void HealthPanel::runHealthCheck() {
  healthButton_->setEnabled(false);
  healthJob_ = engine_.submit(QStringLiteral("health"));
}

void HealthPanel::onStarted() {
  emit pythonStateChanged(QStringLiteral("Python: running"));
  runHealthCheck();
}

void HealthPanel::onStartFailed(const QString& reason) {
  emit pythonStateChanged(QStringLiteral("Python: unavailable"));
  healthError_->setText(reason);
  healthError_->show();
}

void HealthPanel::onJobFinished(const JobResult& result) {
  if (result.id == healthJob_) showHealth(result);
}

void HealthPanel::showHealth(const JobResult& result) {
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

}  // namespace rq
