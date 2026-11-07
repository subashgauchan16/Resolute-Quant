// Drives the Live page with the real Python and a synthetic feed. No provider is contacted.
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableView>
#include <QtTest>

#include "app/CandleChart.h"
#include "app/LivePanel.h"
#include "app/Theme.h"
#include "bridge/PythonEngine.h"

namespace {

constexpr qint64 kMaxAllowedGapMs = 250;  // Set before measuring, as on the Python Health page.

rq::Candle candleAt(qint64 time, double close = 11.0) {
  rq::Candle candle;
  candle.time = time;
  candle.open = 10.0;
  candle.high = 12.0;
  candle.low = 9.0;
  candle.close = close;
  candle.volume = 5.0;
  candle.hasVolume = true;
  return candle;
}

}  // namespace

class LiveUiTest : public QObject {
  Q_OBJECT

 private slots:
  void initTestCase();
  void nothingConnectsUntilGoLive();
  void goLiveFillsPricesAndChart();
  void windowStaysResponsiveWhileStreaming();
  void chartBarKeepsUpdating();
  void selectingAnotherPairSwapsTheChart();
  void changingTimeframeReloads();
  void addAndRemovePairs();
  void refusedPairsShowPlainMessages();
  void reconnectReloadsTheChart();
  void failedChartShowsMessageAndNoOldBars();
  void stopMakesPricesNotLive();
  void goLiveAgainWorks();
  void chartUpdateLatestRules();
  void shutdownWhileStreamingIsClean();

 private:
  template <typename T>
  T* find(const char* name) {
    T* widget = panel_->findChild<T*>(QString::fromLatin1(name));
    if (widget == nullptr) qFatal("Widget not found: %s", name);
    return widget;
  }
  QString cell(int row, int column) { return find<QTableView>("liveTable")->model()->index(row, column).data().toString(); }
  int rowCount() { return find<QTableView>("liveTable")->model()->rowCount(); }
  void selectRow(int row) { find<QTableView>("liveTable")->selectRow(row); }
  quint64 restCalls();

  std::unique_ptr<rq::PythonEngine> engine_;
  std::unique_ptr<rq::LivePanel> panel_;
};

quint64 LiveUiTest::restCalls() {
  QSignalSpy finished(engine_.get(), &rq::PythonEngine::jobFinished);
  const quint64 id = engine_->submit(QStringLiteral("_live_rest_calls_for_test"));
  for (int i = 0; i < 200; ++i) {
    for (const auto& entry : finished) {
      const auto result = entry.at(0).value<rq::JobResult>();
      if (result.id == id) return result.value["calls"].toInteger();
    }
    finished.wait(25);
  }
  return 0;
}

void LiveUiTest::initTestCase() {
  qApp->setStyleSheet(rq::applicationStyleSheet());
  engine_ = std::make_unique<rq::PythonEngine>(rq::PythonEngine::locate());
  panel_ = std::make_unique<rq::LivePanel>(*engine_, QStringLiteral("_live_start_fixture_for_test"));
  panel_->resize(1100, 700);
  panel_->show();
  QVERIFY(QTest::qWaitForWindowExposed(panel_.get()));
  engine_->start();
  QTRY_VERIFY_WITH_TIMEOUT(find<QPushButton>("liveStart")->isEnabled(), 30000);
}

void LiveUiTest::nothingConnectsUntilGoLive() {
  QVERIFY(!panel_->isLive());
  QVERIFY(find<QLabel>("liveStatus")->text().startsWith("Not connected"));
  QVERIFY(!find<QPushButton>("liveStop")->isEnabled());
  QCOMPARE(find<rq::CandleChart>("liveChart")->candleCount(), 0);
  QVERIFY(cell(0, 1).isEmpty());  // No price is shown before there is one.
  QVERIFY(rowCount() >= 2);
  QCOMPARE(find<QComboBox>("liveInterval")->currentText(), QStringLiteral("1M"));
}

void LiveUiTest::goLiveFillsPricesAndChart() {
  QTest::mouseClick(find<QPushButton>("liveStart"), Qt::LeftButton);
  QTRY_VERIFY_WITH_TIMEOUT(panel_->isLive(), 10000);
  QTRY_VERIFY_WITH_TIMEOUT(!cell(0, 1).isEmpty(), 10000);
  QVERIFY(cell(0, 3).toDouble() < cell(0, 4).toDouble());  // Bid below ask.
  QTRY_VERIFY_WITH_TIMEOUT(find<rq::CandleChart>("liveChart")->candleCount() > 0, 10000);
  QTRY_VERIFY(find<QLabel>("liveStatus")->text().startsWith("Live"));
  QCOMPARE(find<QLabel>("liveChartTitle")->text(), cell(0, 0));
  QVERIFY(!find<QPushButton>("liveStart")->isEnabled());
  QVERIFY(find<QPushButton>("liveStop")->isEnabled());
}

void LiveUiTest::windowStaysResponsiveWhileStreaming() {
  QElapsedTimer clock;
  qint64 largest = 0;
  QTimer timer;
  connect(&timer, &QTimer::timeout, &timer, [&] { largest = std::max(largest, clock.restart()); });
  clock.start();
  timer.start(20);
  QTest::qWait(1500);
  timer.stop();
  qInfo("Longest gap between window updates while streaming: %lld ms", largest);
  QVERIFY2(largest < kMaxAllowedGapMs, "The window stalled while streaming");
}

void LiveUiTest::chartBarKeepsUpdating() {
  auto* chart = find<rq::CandleChart>("liveChart");
  const QString before = chart->readout(chart->candleCount() - 1);
  QTRY_VERIFY_WITH_TIMEOUT(chart->readout(chart->candleCount() - 1) != before, 5000);
}

void LiveUiTest::selectingAnotherPairSwapsTheChart() {
  auto* chart = find<rq::CandleChart>("liveChart");
  selectRow(1);
  QCOMPARE(chart->candleCount(), 0);  // Cleared at once, so old bars never sit under the new name.
  QCOMPARE(find<QLabel>("liveChartTitle")->text(), cell(1, 0));
  QTRY_VERIFY_WITH_TIMEOUT(chart->candleCount() > 0, 10000);
}

void LiveUiTest::changingTimeframeReloads() {
  auto* chart = find<rq::CandleChart>("liveChart");
  auto* interval = find<QComboBox>("liveInterval");
  interval->setCurrentIndex(interval->findText("1H"));
  QCOMPARE(chart->candleCount(), 0);
  QTRY_VERIFY_WITH_TIMEOUT(chart->candleCount() > 0, 10000);
  interval->setCurrentIndex(interval->findText("1M"));
  QTRY_VERIFY_WITH_TIMEOUT(chart->candleCount() > 0, 10000);
}

void LiveUiTest::addAndRemovePairs() {
  const int before = rowCount();
  auto* field = find<QLineEdit>("liveAddField");
  field->setText("ltc-usd");
  QTest::mouseClick(find<QPushButton>("liveAdd"), Qt::LeftButton);
  QTRY_COMPARE_WITH_TIMEOUT(rowCount(), before + 1, 5000);
  QCOMPARE(cell(before, 0), QStringLiteral("LTC-USD"));
  QVERIFY(field->text().isEmpty());
  QTRY_VERIFY_WITH_TIMEOUT(!cell(before, 1).isEmpty(), 5000);  // The new pair starts streaming.

  selectRow(before);
  QTest::mouseClick(find<QPushButton>("liveRemove"), Qt::LeftButton);
  QCOMPARE(rowCount(), before);
  QVERIFY(find<QTableView>("liveTable")->currentIndex().isValid());
  selectRow(1);
}

void LiveUiTest::refusedPairsShowPlainMessages() {
  const int before = rowCount();
  auto* field = find<QLineEdit>("liveAddField");
  auto* message = find<QLabel>("liveErrorMessage");
  field->setText("not a pair");
  QTest::mouseClick(find<QPushButton>("liveAdd"), Qt::LeftButton);
  QTRY_VERIFY_WITH_TIMEOUT(message->text().contains("not a Coinbase pair"), 5000);
  QVERIFY(!find<QPushButton>("liveErrorToggle")->isVisible());

  field->setText("NOPE-USD");
  QTest::mouseClick(find<QPushButton>("liveAdd"), Qt::LeftButton);
  QTRY_VERIFY_WITH_TIMEOUT(message->text().contains("does not list"), 5000);
  QCOMPARE(rowCount(), before);
  QVERIFY(find<QPushButton>("liveAdd")->isEnabled());
  field->clear();
}

void LiveUiTest::reconnectReloadsTheChart() {
  const quint64 before = restCalls();
  QVERIFY(before > 0);
  engine_->submit(QStringLiteral("_live_drop_fixture_for_test"));
  QTRY_VERIFY_WITH_TIMEOUT(find<QLabel>("liveStatus")->text().startsWith("Disconnected") || restCalls() > before, 5000);
  QTRY_VERIFY_WITH_TIMEOUT(restCalls() > before, 5000);
  QTRY_VERIFY_WITH_TIMEOUT(panel_->isLive(), 5000);
}

void LiveUiTest::failedChartShowsMessageAndNoOldBars() {
  auto* chart = find<rq::CandleChart>("liveChart");
  auto* field = find<QLineEdit>("liveAddField");
  field->setText("FAIL-USD");
  QTest::mouseClick(find<QPushButton>("liveAdd"), Qt::LeftButton);
  const int last = rowCount();
  QTRY_COMPARE_WITH_TIMEOUT(rowCount(), last + 1, 5000);
  selectRow(last);
  auto* message = find<QLabel>("liveErrorMessage");
  QTRY_VERIFY_WITH_TIMEOUT(message->isVisible() && message->text().contains("Could not reach"), 5000);
  QCOMPARE(chart->candleCount(), 0);

  // Prices still stream, and a working pair recovers the chart and clears the message.
  selectRow(0);
  QTRY_VERIFY_WITH_TIMEOUT(chart->candleCount() > 0, 5000);
  QVERIFY(!message->isVisible());
  selectRow(last);
  QTest::mouseClick(find<QPushButton>("liveRemove"), Qt::LeftButton);
  selectRow(0);
  QTRY_VERIFY_WITH_TIMEOUT(chart->candleCount() > 0, 5000);
}

void LiveUiTest::stopMakesPricesNotLive() {
  QTest::mouseClick(find<QPushButton>("liveStop"), Qt::LeftButton);
  QVERIFY(!panel_->isLive());
  QVERIFY(find<QLabel>("liveStatus")->text().startsWith("Not connected"));
  QTRY_VERIFY_WITH_TIMEOUT(find<QPushButton>("liveStart")->isEnabled(), 5000);
  QVERIFY(!find<QPushButton>("liveStop")->isEnabled());

  // The price stays visible for reference, and greys out instead of looking current.
  const QString price = cell(0, 1);
  QVERIFY(!price.isEmpty());
  const QVariant brush = find<QTableView>("liveTable")->model()->index(0, 1).data(Qt::ForegroundRole);
  QVERIFY(brush.isValid());
  QTest::qWait(400);
  QCOMPARE(cell(0, 1), price);
}

void LiveUiTest::goLiveAgainWorks() {
  QTest::mouseClick(find<QPushButton>("liveStart"), Qt::LeftButton);
  QTRY_VERIFY_WITH_TIMEOUT(panel_->isLive(), 10000);
  QTRY_VERIFY_WITH_TIMEOUT(find<rq::CandleChart>("liveChart")->candleCount() > 0, 10000);
}

void LiveUiTest::chartUpdateLatestRules() {
  rq::CandleChart chart;
  chart.resize(800, 400);
  chart.updateLatest(candleAt(100));  // Nothing loaded yet, so nothing to extend.
  QCOMPARE(chart.candleCount(), 0);

  QVector<rq::Candle> candles;
  for (int i = 0; i < 100; ++i) candles.push_back(candleAt(1000 + i * 60));
  chart.setCandles(candles, true);
  const qint64 last = candles.last().time;

  chart.updateLatest(candleAt(last, 12.0));  // The forming bar changes in place.
  QCOMPARE(chart.candleCount(), 100);
  QVERIFY(chart.readout(99).contains("C 12"));

  chart.updateLatest(candleAt(last - 60, 1.0));  // An older bar is ignored.
  QVERIFY(!chart.readout(98).contains("C 1 "));

  chart.updateLatest(candleAt(last + 60));  // Showing everything keeps showing everything.
  QCOMPARE(chart.candleCount(), 101);
  QCOMPARE(chart.visibleCount(), 101);

  chart.zoomBy(2.0);  // Zoomed in on the newest bars, the view follows them.
  const int zoomed = chart.visibleCount();
  chart.updateLatest(candleAt(last + 120));
  QCOMPARE(chart.visibleCount(), zoomed);
  QCOMPARE(chart.firstVisible() + chart.visibleCount(), 102);

  chart.panBy(-20);  // Looking back, new bars don't move the view.
  const int first = chart.firstVisible();
  chart.updateLatest(candleAt(last + 180));
  QCOMPARE(chart.firstVisible(), first);
  QCOMPARE(chart.candleCount(), 103);
}

void LiveUiTest::shutdownWhileStreamingIsClean() {
  QVERIFY(panel_->isLive());
  QElapsedTimer clock;
  clock.start();
  QVERIFY(engine_->shutdown());
  qInfo("Shutdown while streaming took %lld ms", clock.elapsed());
  QVERIFY(clock.elapsed() < rq::PythonEngine::kShutdownGraceMs);
  panel_.reset();
}

QTEST_MAIN(LiveUiTest)
#include "test_live_ui.moc"
