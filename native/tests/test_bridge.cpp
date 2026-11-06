// Exercises PythonEngine against the real private Python.
#include <QJsonArray>
#include <QSignalSpy>
#include <QtTest>
#include <cmath>
#include <numeric>
#include <vector>

#include "bridge/PythonEngine.h"

using rq::JobResult;
using rq::JobStatus;
using rq::PythonEngine;

class BridgeTest : public QObject {
  Q_OBJECT

 private slots:
  void initTestCase();
  void statisticsMatchAnIndependentCalculation();
  void emptyInputReportsValidationMessage();
  void pythonExceptionCarriesDiagnosticsAndTheNextJobWorks();
  void systemExitFailsTheJobAndNotTheApp();
  void unknownJobIsAnError();
  void interpreterIsIsolated();
  void longJobReportsProgressAndCancels();
  void queuedJobsRunInOrderAndCancelRemovesOne();
  void missingRuntimeIsReportedWithoutCrashing();
  void incompleteRuntimeIsReportedWithoutCrashing();
  void shutdownStopsARunningJobAndRefusesNewOnes();

 private:
  JobResult waitFor(quint64 id, int timeoutMs = 30000);
  std::unique_ptr<PythonEngine> engine_;
  std::unique_ptr<QSignalSpy> finished_;  // Listens from the start, so no result can be missed.
};

JobResult BridgeTest::waitFor(quint64 id, int timeoutMs) {
  QElapsedTimer clock;
  clock.start();
  while (clock.elapsed() < timeoutMs) {
    for (const auto& entry : *finished_) {
      const auto result = entry.at(0).value<JobResult>();
      if (result.id == id) return result;
    }
    finished_->wait(50);
  }
  return {};
}

void BridgeTest::initTestCase() {
  // These would mislead an interpreter that was not isolated.
  qputenv("PYTHONHOME", "C:\\bogus-python-home");
  qputenv("PYTHONPATH", "C:\\bogus-python-path");

  engine_ = std::make_unique<PythonEngine>(PythonEngine::locate());
  finished_ = std::make_unique<QSignalSpy>(engine_.get(), &PythonEngine::jobFinished);
  QSignalSpy started(engine_.get(), &PythonEngine::started);
  QSignalSpy failed(engine_.get(), &PythonEngine::startFailed);
  engine_->start();
  QTRY_VERIFY_WITH_TIMEOUT(started.count() + failed.count() > 0, 30000);
  if (!failed.isEmpty()) QFAIL(qPrintable(failed.at(0).at(0).toString()));
}

void BridgeTest::statisticsMatchAnIndependentCalculation() {
  const std::vector<double> values{1.5, -2.25, 3.125, 1000.0, 0.0625, 7.0, -13.5};
  const double mean = std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
  double squares = 0.0;
  for (const double value : values) squares += (value - mean) * (value - mean);
  const double stdDev = std::sqrt(squares / static_cast<double>(values.size() - 1));

  QStringList text;
  for (const double value : values) text << QString::number(value, 'g', 17);
  const JobResult result = waitFor(engine_->submit("summary_statistics", {{"text", text.join(", ")}}));

  QCOMPARE(result.status, JobStatus::Ok);
  QCOMPARE(result.value["count"].toInt(), static_cast<int>(values.size()));
  QVERIFY(std::abs(result.value["mean"].toDouble() - mean) < 1e-9);
  QVERIFY(std::abs(result.value["sample_std_dev"].toDouble() - stdDev) < 1e-9);
}

void BridgeTest::emptyInputReportsValidationMessage() {
  const JobResult result = waitFor(engine_->submit("summary_statistics", {{"text", ""}}));
  QCOMPARE(result.status, JobStatus::Error);
  QCOMPARE(result.errorType, QStringLiteral("ValueError"));
  QCOMPARE(result.message, QStringLiteral("Enter at least two numbers."));
}

void BridgeTest::pythonExceptionCarriesDiagnosticsAndTheNextJobWorks() {
  const JobResult failed = waitFor(engine_->submit("summary_statistics", {{"text", "1, abc, 3"}}));
  QCOMPARE(failed.status, JobStatus::Error);
  QCOMPARE(failed.errorType, QStringLiteral("ValueError"));
  QVERIFY2(failed.message.contains("abc"), qPrintable(failed.message));
  QVERIFY2(failed.traceback.contains("calculations.py"), qPrintable(failed.traceback));
  QVERIFY2(failed.traceback.contains("_parse_numbers"), qPrintable(failed.traceback));

  const JobResult next = waitFor(engine_->submit("summary_statistics", {{"text", "1, 3"}}));
  QCOMPARE(next.status, JobStatus::Ok);
  QCOMPARE(next.value["mean"].toDouble(), 2.0);
}

void BridgeTest::systemExitFailsTheJobAndNotTheApp() {
  const JobResult failed = waitFor(engine_->submit("_exit_for_test"));
  QCOMPARE(failed.status, JobStatus::Error);
  QCOMPARE(failed.errorType, QStringLiteral("SystemExit"));
  QCOMPARE(waitFor(engine_->submit("health")).status, JobStatus::Ok);
}

void BridgeTest::unknownJobIsAnError() {
  const JobResult result = waitFor(engine_->submit("no_such_job"));
  QCOMPARE(result.status, JobStatus::Error);
  QVERIFY(result.message.contains("no_such_job"));
}

void BridgeTest::interpreterIsIsolated() {
  const JobResult result = waitFor(engine_->submit("health"));
  QCOMPARE(result.status, JobStatus::Ok);
  QVERIFY(result.value["isolated"].toBool());
  QVERIFY(result.value["ignores_environment"].toBool());
  QVERIFY(!result.value["user_site_enabled"].toBool());
  QCOMPARE(result.value["python_version"].toString(), QStringLiteral("3.13.17"));
  QCOMPARE(result.value["numpy_version"].toString(), QStringLiteral("2.5.4"));

  const QString runtime = QDir::cleanPath(engine_->paths().runtimeDir);
  const QString engineDir = QDir::cleanPath(engine_->paths().engineDir);
  QCOMPARE(QDir::cleanPath(result.value["prefix"].toString()), runtime);
  const QJsonArray path = result.value["path"].toArray();
  QVERIFY(!path.isEmpty());
  for (const auto& entry : path) {
    const QString clean = QDir::cleanPath(entry.toString());
    QVERIFY2(clean.startsWith(runtime, Qt::CaseInsensitive) || clean.startsWith(engineDir, Qt::CaseInsensitive),
             qPrintable(clean));
  }
}

void BridgeTest::longJobReportsProgressAndCancels() {
  QSignalSpy progress(engine_.get(), &PythonEngine::progress);
  const quint64 id = engine_->submit("long_demo", {{"seconds", 60.0}});
  QTRY_VERIFY_WITH_TIMEOUT(progress.count() >= 2, 15000);
  QCOMPARE(progress.at(0).at(0).toULongLong(), id);
  QVERIFY(progress.last().at(1).toDouble() > 0.0);

  QElapsedTimer clock;
  clock.start();
  engine_->cancel(id);
  const JobResult result = waitFor(id, 10000);
  QCOMPARE(result.status, JobStatus::Cancelled);
  QVERIFY2(clock.elapsed() < 5000, "Cancel took too long");
  QCOMPARE(waitFor(engine_->submit("health")).status, JobStatus::Ok);
}

void BridgeTest::queuedJobsRunInOrderAndCancelRemovesOne() {
  QSignalSpy finished(engine_.get(), &PythonEngine::jobFinished);
  const quint64 first = engine_->submit("long_demo", {{"seconds", 0.5}});
  const quint64 second = engine_->submit("summary_statistics", {{"text", "1, 2"}});
  const quint64 removed = engine_->submit("summary_statistics", {{"text", "3, 4"}});
  const quint64 third = engine_->submit("summary_statistics", {{"text", "5, 6"}});
  engine_->cancel(removed);

  QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 4, 15000);
  QList<quint64> ran;
  for (const auto& entry : finished) {
    const auto result = entry.at(0).value<JobResult>();
    if (result.id == removed) {
      QCOMPARE(result.status, JobStatus::Cancelled);
    } else {
      QCOMPARE(result.status, JobStatus::Ok);
      ran << result.id;
    }
  }
  QCOMPARE(ran, (QList<quint64>{first, second, third}));
}

void BridgeTest::missingRuntimeIsReportedWithoutCrashing() {
  PythonEngine::Paths paths = PythonEngine::locate();
  paths.runtimeDir = QStringLiteral("C:/rq-no-such-runtime");
  PythonEngine missing(paths);
  QSignalSpy failed(&missing, &PythonEngine::startFailed);
  QSignalSpy finished(&missing, &PythonEngine::jobFinished);
  missing.start();
  QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 5000);
  QVERIFY(failed.at(0).at(0).toString().contains("rq-no-such-runtime"));

  missing.submit("health");
  QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
  const auto result = finished.at(0).at(0).value<JobResult>();
  QCOMPARE(result.status, JobStatus::Error);
  QCOMPARE(result.errorType, QStringLiteral("PythonUnavailable"));
  QVERIFY(missing.shutdown());
}

void BridgeTest::incompleteRuntimeIsReportedWithoutCrashing() {
  QTemporaryDir folder;
  QVERIFY(folder.isValid());
  PythonEngine::Paths paths = PythonEngine::locate();
  paths.runtimeDir = folder.path();
  PythonEngine incomplete(paths);
  QSignalSpy failed(&incomplete, &PythonEngine::startFailed);
  incomplete.start();
  QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 5000);
  QVERIFY(failed.at(0).at(0).toString().contains("python313.dll"));
  QVERIFY(incomplete.shutdown());
}

// Last, because the interpreter cannot restart in this process.
void BridgeTest::shutdownStopsARunningJobAndRefusesNewOnes() {
  QSignalSpy progress(engine_.get(), &PythonEngine::progress);
  engine_->submit("long_demo", {{"seconds", 60.0}});
  QTRY_VERIFY_WITH_TIMEOUT(progress.count() >= 1, 15000);

  QElapsedTimer clock;
  clock.start();
  QVERIFY(engine_->shutdown());
  QVERIFY2(clock.elapsed() < PythonEngine::kShutdownGraceMs, "Shutdown used the whole grace period");
  QCOMPARE(engine_->submit("health"), quint64(0));
}

QTEST_MAIN(BridgeTest)
#include "test_bridge.moc"
