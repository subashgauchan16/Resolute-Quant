#pragma once

#include <QMainWindow>

class QLabel;
class QListWidget;
class QStackedWidget;

namespace rq {

// The shell: sidebar navigation on the left, the selected page on the right, and a status bar.
class MainWindow : public QMainWindow {
  Q_OBJECT

 public:
  explicit MainWindow(QWidget* parent = nullptr);

 private:
  void addPage(const QString& title, QWidget* page);

  QListWidget* navigation_;
  QStackedWidget* pages_;
};

}  // namespace rq
