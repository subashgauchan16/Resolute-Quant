#pragma once

#include <QMainWindow>

class QLabel;
class QListWidget;
class QStackedWidget;

namespace rq {

class PythonEngine;

// The shell: sidebar navigation on the left, the selected page on the right, and a status bar.
class MainWindow : public QMainWindow {
  Q_OBJECT

 public:
  explicit MainWindow(PythonEngine& engine, QWidget* parent = nullptr);

 private:
  void addPage(const QString& title, QWidget* page);

  QListWidget* navigation_;
  QStackedWidget* pages_;
  QLabel* pythonState_;
};

}  // namespace rq
