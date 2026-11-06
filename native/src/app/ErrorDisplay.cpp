#include "app/ErrorDisplay.h"

#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace rq {

ErrorDisplay::ErrorDisplay(const QString& name, QWidget* parent)
    : QWidget(parent), message_(new QLabel), toggle_(new QPushButton), details_(new QPlainTextEdit) {
  setObjectName(name);
  message_->setObjectName(name + QStringLiteral("Message"));
  message_->setStyleSheet(QStringLiteral("color: #ef9a8f;"));
  message_->setWordWrap(true);
  message_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  toggle_->setObjectName(name + QStringLiteral("Toggle"));
  details_->setObjectName(name + QStringLiteral("Details"));
  details_->setReadOnly(true);
  details_->setMaximumHeight(130);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(message_);
  layout->addWidget(toggle_, 0, Qt::AlignLeft);
  layout->addWidget(details_);

  connect(toggle_, &QPushButton::clicked, this, &ErrorDisplay::toggleDetails);
  clearError();
}

void ErrorDisplay::showError(const QString& message, const QString& details) {
  message_->setText(message);
  details_->setPlainText(details);
  details_->hide();
  toggle_->setText(QStringLiteral("Show details"));
  toggle_->setVisible(!details.isEmpty());
  show();
}

void ErrorDisplay::clearError() {
  hide();
  message_->clear();
  details_->clear();
  details_->hide();
  toggle_->hide();
}

void ErrorDisplay::toggleDetails() {
  const bool showing = !details_->isVisibleTo(this);
  details_->setVisible(showing);
  toggle_->setText(showing ? QStringLiteral("Hide details") : QStringLiteral("Show details"));
}

}  // namespace rq
