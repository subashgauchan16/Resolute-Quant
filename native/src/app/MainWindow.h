#pragma once

#include <QMainWindow>

#include "app/TerminalPage.h"

class QStackedWidget;

namespace rq {

class HealthPanel;
class HistoryPanel;
class PythonEngine;

// The terminal fills the window. History and Python Health open over it, and close back to it.
class MainWindow : public QMainWindow {
  Q_OBJECT

 public:
  explicit MainWindow(PythonEngine& engine, TerminalOptions options = {}, QWidget* parent = nullptr);

  TerminalPage* terminal() const { return terminal_; }
  void showTerminal();
  void showHistory(const QString& symbol, bool isMarket);
  void showHealth();
  QWidget* currentPage() const;

 private:
  QStackedWidget* pages_;
  TerminalPage* terminal_;
  HistoryPanel* history_;
  QWidget* health_;
};

}  // namespace rq
