#include <QApplication>

#include "app/MainWindow.h"
#include "app/Theme.h"

int main(int argc, char* argv[]) {
  QApplication app(argc, argv);
  QApplication::setApplicationName(QStringLiteral("ResoluteQuant"));
  app.setStyleSheet(rq::applicationStyleSheet());

  rq::MainWindow window;
  window.show();
  return QApplication::exec();
}
