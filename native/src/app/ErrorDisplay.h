#pragma once

#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace rq {

// A plain error message, with any technical details hidden behind a button.
class ErrorDisplay : public QWidget {
  Q_OBJECT

 public:
  // Child widgets are named <name>Message, <name>Toggle and <name>Details.
  explicit ErrorDisplay(const QString& name, QWidget* parent = nullptr);

  void showError(const QString& message, const QString& details = {});
  void clearError();

 private:
  void toggleDetails();

  QLabel* message_;
  QPushButton* toggle_;
  QPlainTextEdit* details_;
};

}  // namespace rq
