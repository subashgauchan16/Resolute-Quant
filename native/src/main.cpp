#include <QApplication>
#include <QCommandLineParser>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <cmath>
#include <cstdio>

#include "app/Logging.h"
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "bridge/PythonEngine.h"

namespace {

constexpr int kExitPythonUnavailable = 2;
constexpr int kExitSmokeFailed = 3;
constexpr int kSmokeTimeoutMs = 60000;

void printLine(const QJsonObject& line) {
  std::fputs(QJsonDocument(line).toJson(QJsonDocument::Compact).constData(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

// Opens the app normally, runs a health check and a calculation through Python, prints the outcome
// as one JSON line and quits through the normal shutdown path.
void runSmokeTest(rq::PythonEngine& engine) {
  auto* healthJob = new quint64(0);
  auto* calculationJob = new quint64(0);
  auto* packagesJob = new quint64(0);
  auto* storageJob = new quint64(0);
  auto* report = new QJsonObject;

  QObject::connect(&engine, &rq::PythonEngine::started, &engine, [&engine, healthJob] {
    *healthJob = engine.submit(QStringLiteral("health"));
  });
  QObject::connect(&engine, &rq::PythonEngine::startFailed, &engine, [](const QString& reason) {
    printLine({{"python", "unavailable"}, {"reason", reason}});
    QApplication::exit(kExitPythonUnavailable);
  });
  QObject::connect(&engine, &rq::PythonEngine::jobFinished, &engine,
                   [&engine, healthJob, calculationJob, packagesJob, storageJob, report](const rq::JobResult& result) {
                     if (result.status != rq::JobStatus::Ok &&
                         (result.id == *healthJob || result.id == *calculationJob || result.id == *packagesJob || result.id == *storageJob)) {
                       printLine({{"python", "running"}, {"ok", false}, {"job", result.job}, {"error", result.message}});
                       QApplication::exit(kExitSmokeFailed);
                     } else if (result.id == *healthJob) {
                       *report = result.value;
                       *calculationJob = engine.submit(QStringLiteral("summary_statistics"),
                                                       {{"text", "2, 4, 4, 4, 5, 5, 7, 9"}});
                     } else if (result.id == *calculationJob) {
                       // The expected mean is 5, and the sample variance is 32 / 7.
                       const bool correct = result.value["mean"].toDouble() == 5.0 &&
                                            std::abs(result.value["sample_std_dev"].toDouble() - std::sqrt(32.0 / 7.0)) < 1e-12;
                       const bool isolated = (*report)["isolated"].toBool() && !(*report)["numpy_version"].toString().isEmpty();
                       (*report)["ok"] = correct && isolated;
                       (*report)["calculation"] = result.value;
                       // Quitting with these libraries loaded proves shutdown still works.
                       *packagesJob = engine.submit(QStringLiteral("_import_market_for_test"));
                     } else if (result.id == *packagesJob) {
                       (*report)["packages"] = result.value;
                       *storageJob = engine.submit(QStringLiteral("_storage_self_test_for_test"));
                     } else if (result.id == *storageJob) {
                       const bool ok = (*report)["ok"].toBool() && result.value["ok"].toBool();
                       printLine({{"python", "running"},
                                  {"ok", ok},
                                  {"prefix", (*report)["prefix"]},
                                  {"python_version", (*report)["python_version"]},
                                  {"numpy_version", (*report)["numpy_version"]},
                                  {"isolated", (*report)["isolated"]},
                                  {"calculation", (*report)["calculation"]},
                                  {"packages", (*report)["packages"]},
                                  {"storage", result.value}});
                       QApplication::exit(ok ? 0 : kExitSmokeFailed);
                     }
                   });
  QTimer::singleShot(kSmokeTimeoutMs, qApp, [] { QApplication::exit(kExitSmokeFailed); });
}

// Starts a job that ignores cancel, then quits, to exercise the forced shutdown path.
void runStuckShutdownTest(rq::PythonEngine& engine) {
  QObject::connect(&engine, &rq::PythonEngine::started, &engine, [&engine] {
    engine.submit(QStringLiteral("_stuck_for_test"), {{"seconds", 30.0}});
    QTimer::singleShot(1000, qApp, [] { QApplication::exit(0); });
  });
}

}  // namespace

int main(int argc, char* argv[]) {
  QApplication app(argc, argv);
  QApplication::setApplicationName(QStringLiteral("ResoluteQuant"));
  rq::installFileLogging();
  rq::loadFonts();
  app.setStyleSheet(rq::applicationStyleSheet());

  QCommandLineParser parser;
  parser.addHelpOption();
  const QCommandLineOption smokeTest(QStringLiteral("smoke-test"),
                                     QStringLiteral("Open the window, run Python jobs, print the result and exit."));
  const QCommandLineOption stuckTest(QStringLiteral("test-stuck-shutdown"),
                                     QStringLiteral("Quit during a job that ignores cancel."));
  const QCommandLineOption offline(QStringLiteral("offline"),
                                   QStringLiteral("Open without contacting any data provider."));
  const QCommandLineOption runtime(QStringLiteral("python-runtime"),
                                   QStringLiteral("Use the private Python in this folder."), QStringLiteral("folder"));
  parser.addOptions({smokeTest, stuckTest, offline, runtime});
  parser.process(app);

  rq::PythonEngine engine(rq::PythonEngine::locate(parser.value(runtime)));
  rq::TerminalOptions options;
  options.connect = !parser.isSet(offline);
  options.showIntro = !parser.isSet(offline) && !parser.isSet(smokeTest) && !parser.isSet(stuckTest);
  rq::MainWindow window(engine, options);
  window.show();

  if (parser.isSet(smokeTest)) runSmokeTest(engine);
  if (parser.isSet(stuckTest)) runStuckShutdownTest(engine);
  engine.start();

  const int code = QApplication::exec();
  engine.shutdownOrTerminate();
  return code;
}
