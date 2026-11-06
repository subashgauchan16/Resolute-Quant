#include <QApplication>
#include <QCommandLineParser>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <cstdio>

#include "app/Logging.h"
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "bridge/PythonEngine.h"

namespace {

constexpr int kExitPythonUnavailable = 2;
constexpr int kExitSmokeFailed = 3;
constexpr int kSmokeTimeoutMs = 60000;

void printLine(const QByteArray& line) {
  std::fputs(line.constData(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

// Starts Python, runs a real job, prints the outcome as one JSON line, then quits normally.
void runSmokeTest(rq::PythonEngine& engine) {
  QObject::connect(&engine, &rq::PythonEngine::started, &engine, [&engine] { engine.submit(QStringLiteral("health")); });
  QObject::connect(&engine, &rq::PythonEngine::startFailed, &engine, [](const QString& reason) {
    printLine(QJsonDocument(QJsonObject{{"python", "unavailable"}, {"reason", reason}}).toJson(QJsonDocument::Compact));
    QApplication::exit(kExitPythonUnavailable);
  });
  QObject::connect(&engine, &rq::PythonEngine::jobFinished, &engine, [](const rq::JobResult& result) {
    const bool ok = result.status == rq::JobStatus::Ok;
    QJsonObject line{{"python", "running"}, {"job", result.job}, {"ok", ok}};
    if (ok) {
      line["value"] = result.value;
    } else {
      line["error"] = result.message;
    }
    printLine(QJsonDocument(line).toJson(QJsonDocument::Compact));
    QApplication::exit(ok ? 0 : kExitSmokeFailed);
  });
  QTimer::singleShot(kSmokeTimeoutMs, qApp, [] { QApplication::exit(kExitSmokeFailed); });
}

}  // namespace

int main(int argc, char* argv[]) {
  QApplication app(argc, argv);
  QApplication::setApplicationName(QStringLiteral("ResoluteQuant"));
  QApplication::setOrganizationName(QString());
  rq::installFileLogging();
  app.setStyleSheet(rq::applicationStyleSheet());

  QCommandLineParser parser;
  parser.addHelpOption();
  const QCommandLineOption smokeTest(QStringLiteral("smoke-test"),
                                     QStringLiteral("Open the window, run one Python job, print the result and exit."));
  const QCommandLineOption runtime(QStringLiteral("python-runtime"),
                                   QStringLiteral("Use the private Python in this folder."), QStringLiteral("folder"));
  parser.addOptions({smokeTest, runtime});
  parser.process(app);

  rq::PythonEngine engine(rq::PythonEngine::locate(parser.value(runtime)));
  rq::MainWindow window(engine);
  window.show();

  if (parser.isSet(smokeTest)) runSmokeTest(engine);
  engine.start();

  const int code = QApplication::exec();
  engine.shutdownOrTerminate();
  return code;
}
