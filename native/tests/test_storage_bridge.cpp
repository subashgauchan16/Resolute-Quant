// Asks for storage status from Qt through the real embedded Python, while a separate Python process writes.
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include "bridge/PythonEngine.h"

using rq::JobResult;
using rq::JobStatus;
using rq::PythonEngine;

namespace {
constexpr qint64 kMaxAllowedGapMs = 250;  // The same limit as the app's other responsiveness tests.
}

class StorageBridgeTest : public QObject {
  Q_OBJECT

 private slots:
  void initTestCase();
  void statusComesBackThroughTheBridge();
  void aRefusedLocationIsAPlainErrorAndCreatesNothing();
  void statusStaysCorrectAndTheWindowLiveWhileAnotherProcessWrites();
  void cleanupTestCase();

 private:
  JobResult status(const QString& folder);
  std::unique_ptr<PythonEngine> engine_;
  std::unique_ptr<QSignalSpy> finished_;
  QTemporaryDir folder_;
};

JobResult StorageBridgeTest::status(const QString& folder) {
  const quint64 id = engine_->submit(QStringLiteral("storage_status"), QJsonObject{{QStringLiteral("data_dir"), folder}});
  QElapsedTimer clock;
  clock.start();
  while (clock.elapsed() < 30000) {
    for (const auto& entry : *finished_) {
      const auto result = entry.at(0).value<JobResult>();
      if (result.id == id) return result;
    }
    finished_->wait(10);
  }
  return {};
}

void StorageBridgeTest::initTestCase() {
  QVERIFY(folder_.isValid());
  engine_ = std::make_unique<PythonEngine>(PythonEngine::locate());
  finished_ = std::make_unique<QSignalSpy>(engine_.get(), &PythonEngine::jobFinished);
  QSignalSpy started(engine_.get(), &PythonEngine::started);
  engine_->start();
  QTRY_VERIFY_WITH_TIMEOUT(started.count() > 0, 30000);
}

void StorageBridgeTest::statusComesBackThroughTheBridge() {
  const QString folder = folder_.filePath(QStringLiteral("first"));
  const JobResult result = status(folder);
  QCOMPARE(result.status, JobStatus::Ok);
  QVERIFY(result.value["ok"].toBool());
  QVERIFY2(result.value["sqlite_version"].toString() >= QStringLiteral("3.51.3"), qPrintable(result.value["sqlite_version"].toString()));
  QCOMPARE(result.value["databases"].toObject()["research"].toObject()["journal_mode"].toString(), QStringLiteral("wal"));
  QCOMPARE(result.value["databases"].toObject()["market"].toObject()["schema_version"].toInt(), 1);
  QVERIFY(QFile::exists(folder + QStringLiteral("/research.sqlite")));
  QVERIFY(QFile::exists(folder + QStringLiteral("/market.sqlite")));
}

void StorageBridgeTest::aRefusedLocationIsAPlainErrorAndCreatesNothing() {
  const QString refused = folder_.filePath(QStringLiteral("OneDrive/data"));
  const JobResult result = status(refused);
  QCOMPARE(result.status, JobStatus::Error);
  QCOMPARE(result.errorType, QStringLiteral("StorageError"));
  QVERIFY2(result.message.contains(QStringLiteral("cloud-synced")), qPrintable(result.message));
  QVERIFY(!QDir(refused).exists());
}

void StorageBridgeTest::statusStaysCorrectAndTheWindowLiveWhileAnotherProcessWrites() {
  const QString folder = folder_.filePath(QStringLiteral("shared"));
  QCOMPARE(status(folder).status, JobStatus::Ok);

  QProcess writer;
  writer.setProgram(engine_->paths().runtimeDir + QStringLiteral("/python.exe"));
  writer.setArguments({QStringLiteral("-I"), QStringLiteral(RQ_TESTS_DIR "/storage_worker.py"), QStringLiteral("forever"), folder});
  writer.start();
  QVERIFY(writer.waitForStarted());
  QTRY_VERIFY_WITH_TIMEOUT(writer.canReadLine() || writer.waitForReadyRead(50), 30000);

  qint64 largestGap = 0;
  QElapsedTimer gapClock;
  gapClock.start();
  QTimer gapTimer;
  connect(&gapTimer, &QTimer::timeout, &gapTimer, [&] { largestGap = std::max(largestGap, gapClock.restart()); });
  gapTimer.start(20);

  int checks = 0;
  int lastSources = -1;
  bool grew = false;
  QElapsedTimer clock;
  clock.start();
  while (clock.elapsed() < 4000) {
    const JobResult result = status(folder);
    QCOMPARE(result.status, JobStatus::Ok);
    QVERIFY2(result.value["ok"].toBool(), "An integrity check failed during writes");
    const int sources = result.value["counts"].toObject()["data_sources"].toInt();
    QVERIFY2(sources >= lastSources, "Committed rows went missing");
    grew = grew || (lastSources >= 0 && sources > lastSources);
    lastSources = sources;
    ++checks;
  }
  gapTimer.stop();
  writer.kill();
  writer.waitForFinished();

  qInfo("%d status checks, %d rows written, longest gap between window updates %lld ms", checks, lastSources, largestGap);
  QVERIFY(checks > 5);
  QVERIFY2(grew, "The other process wrote nothing during the test");
  QVERIFY2(largestGap < kMaxAllowedGapMs, "The window stalled while storage was in use");
}

void StorageBridgeTest::cleanupTestCase() { QVERIFY(engine_->shutdown()); }

QTEST_MAIN(StorageBridgeTest)
#include "test_storage_bridge.moc"
