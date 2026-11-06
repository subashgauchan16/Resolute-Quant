#include "app/MainWindow.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QStackedWidget>
#include <QStatusBar>
#include <QVBoxLayout>

#include "app/HealthPanel.h"

namespace rq {

namespace {

QWidget* makeHomePage() {
  auto* page = new QWidget;
  auto* layout = new QVBoxLayout(page);
  layout->setContentsMargins(24, 20, 24, 20);
  auto* title = new QLabel(QStringLiteral("Resolute Quant"));
  title->setObjectName(QStringLiteral("pageTitle"));
  auto* body = new QLabel(QStringLiteral(
      "Native application foundation. This build proves the Qt window and the embedded Python "
      "engine work together. It has no market data, models or trading functions. The browser "
      "terminal remains the working product."));
  body->setWordWrap(true);
  layout->addWidget(title);
  layout->addWidget(body);
  layout->addStretch();
  return page;
}

}  // namespace

MainWindow::MainWindow(PythonEngine& engine, QWidget* parent)
    : QMainWindow(parent),
      navigation_(new QListWidget),
      pages_(new QStackedWidget),
      pythonState_(new QLabel(QStringLiteral("Python: starting"))) {
  setWindowTitle(QStringLiteral("Resolute Quant"));
  setWindowIcon(QIcon(QStringLiteral(":/assets/img/favicon.png")));
  resize(1000, 680);

  navigation_->setObjectName(QStringLiteral("navigation"));
  auto* brand = new QLabel(QStringLiteral("Resolute Quant"));
  brand->setObjectName(QStringLiteral("brand"));

  auto* sidebar = new QWidget;
  sidebar->setFixedWidth(190);
  auto* sidebarLayout = new QVBoxLayout(sidebar);
  sidebarLayout->setContentsMargins(0, 0, 0, 0);
  sidebarLayout->setSpacing(0);
  sidebarLayout->addWidget(brand);
  sidebarLayout->addWidget(navigation_);

  auto* central = new QWidget;
  auto* layout = new QHBoxLayout(central);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  layout->addWidget(sidebar);
  layout->addWidget(pages_, 1);
  setCentralWidget(central);

  connect(navigation_, &QListWidget::currentRowChanged, pages_, &QStackedWidget::setCurrentIndex);

  auto* healthPanel = new HealthPanel(engine);
  addPage(QStringLiteral("Home"), makeHomePage());
  addPage(QStringLiteral("Python Health"), healthPanel);
  navigation_->setCurrentRow(0);

  pythonState_->setObjectName(QStringLiteral("pythonState"));
  statusBar()->addPermanentWidget(pythonState_);
  connect(healthPanel, &HealthPanel::pythonStateChanged, pythonState_, &QLabel::setText);
}

void MainWindow::addPage(const QString& title, QWidget* page) {
  navigation_->addItem(title);
  pages_->addWidget(page);
}

}  // namespace rq
