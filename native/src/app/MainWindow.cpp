#include "app/MainWindow.h"

#include <QHBoxLayout>
#include <QIcon>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "app/HealthPanel.h"
#include "app/HistoryPanel.h"

namespace rq {

MainWindow::MainWindow(PythonEngine& engine, TerminalOptions options, QWidget* parent)
    : QMainWindow(parent),
      pages_(new QStackedWidget),
      terminal_(new TerminalPage(engine, std::move(options))),
      history_(new HistoryPanel(engine)),
      health_(new QWidget) {
  setWindowTitle(QStringLiteral("Resolute Quant"));
  setWindowIcon(QIcon(QStringLiteral(":/assets/img/favicon.png")));
  setMinimumSize(1000, 640);
  resize(1500, 900);
  setCentralWidget(pages_);

  auto* healthPanel = new HealthPanel(engine);
  auto* healthScroll = new QScrollArea;
  healthScroll->setWidget(healthPanel);
  healthScroll->setWidgetResizable(true);
  healthScroll->setFrameShape(QFrame::NoFrame);
  auto* healthLayout = new QVBoxLayout(health_);
  healthLayout->setContentsMargins(0, 0, 0, 0);
  healthLayout->setSpacing(0);
  auto* closeRow = new QHBoxLayout;
  closeRow->setContentsMargins(0, 10, 24, 0);
  auto* closeHealth = new QPushButton(QStringLiteral("Close"));
  closeHealth->setObjectName(QStringLiteral("healthClose"));
  closeRow->addStretch();
  closeRow->addWidget(closeHealth);
  healthLayout->addLayout(closeRow);
  healthLayout->addWidget(healthScroll, 1);

  pages_->addWidget(terminal_);
  pages_->addWidget(history_);
  pages_->addWidget(health_);

  connect(terminal_, &TerminalPage::historyRequested, this, &MainWindow::showHistory);
  connect(terminal_, &TerminalPage::healthRequested, this, &MainWindow::showHealth);
  connect(history_, &HistoryPanel::closeRequested, this, &MainWindow::showTerminal);
  connect(closeHealth, &QPushButton::clicked, this, &MainWindow::showTerminal);
  auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
  escape->setContext(Qt::WindowShortcut);
  connect(escape, &QShortcut::activatedAmbiguously, this, &MainWindow::showTerminal);
  connect(escape, &QShortcut::activated, this, &MainWindow::showTerminal);
  terminal_->setFocus();
}

void MainWindow::showTerminal() {
  pages_->setCurrentWidget(terminal_);
  terminal_->setFocus();
}

void MainWindow::showHistory(const QString& symbol, bool isMarket) {
  history_->openFor(symbol, isMarket);
  pages_->setCurrentWidget(history_);
}

void MainWindow::showHealth() { pages_->setCurrentWidget(health_); }

QWidget* MainWindow::currentPage() const { return pages_->currentWidget(); }

}  // namespace rq
