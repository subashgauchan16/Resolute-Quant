// Drives the History page and chart with the real Python, using synthetic bars. No provider is contacted.
#include <QComboBox>
#include <QDateEdit>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableView>
#include <QTemporaryDir>
#include <QWheelEvent>
#include <QtTest>

#include "app/CandleChart.h"
#include "app/HistoryPanel.h"
#include "app/Theme.h"
#include "bridge/PythonEngine.h"

namespace {

constexpr int kFixtureBars = 39;  // 40 steps, with one left missing.

QVector<rq::Candle> makeCandles(int count) {
  QVector<rq::Candle> candles;
  for (int i = 0; i < count; ++i) {
    rq::Candle candle;
    candle.time = 1700000000 + i * 86400;
    const bool rising = i % 2 == 0;
    candle.open = rising ? 10.0 : 12.0;
    candle.close = rising ? 12.0 : 10.0;
    candle.high = 13.0;
    candle.low = 9.0;
    candle.volume = 100.0;
    candle.hasVolume = true;
    candles.push_back(candle);
  }
  return candles;
}

bool imageHasColor(const QImage& image, QRgb color) {
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      if (image.pixel(x, y) == color) return true;
    }
  }
  return false;
}

}  // namespace

class HistoryUiTest : public QObject {
  Q_OBJECT

 private slots:
  void initTestCase();
  void optionsFillTheControls();
  void changingSourceChangesTimeframesAndExample();
  void exportIsDisabledUntilSomethingLoads();
  void loadShowsChartTableAndHonestSummary();
  void missingVolumeStaysBlankInTheTable();
  void exportWritesExactlyTheBarsShown();
  void exportFailureIsShownAndKeepsTheBars();
  void badSymbolShowsPlainMessageAndClearsOldBars();
  void chartZoomAndPanStayInBounds();
  void chartWheelAndKeyboard();
  void chartDrawsBothCandleColours();
  void chartHandlesFlatPricesAndNegativePrices();
  void cleanupTestCase();

 private:
  void loadFixture();
  template <typename T>
  T* find(const char* name) {
    T* widget = panel_->findChild<T*>(QString::fromLatin1(name));
    if (widget == nullptr) qFatal("Widget not found: %s", name);
    return widget;
  }

  std::unique_ptr<rq::PythonEngine> engine_;
  std::unique_ptr<rq::HistoryPanel> panel_;
  QTemporaryDir folder_;
};

void HistoryUiTest::initTestCase() {
  qApp->setStyleSheet(rq::applicationStyleSheet());
  engine_ = std::make_unique<rq::PythonEngine>(rq::PythonEngine::locate());
  panel_ = std::make_unique<rq::HistoryPanel>(*engine_, QStringLiteral("_history_fixture_for_test"));
  panel_->resize(1000, 700);
  panel_->show();
  QVERIFY(QTest::qWaitForWindowExposed(panel_.get()));
  engine_->start();
  QTRY_VERIFY_WITH_TIMEOUT(find<QPushButton>("historyLoad")->isEnabled(), 30000);
}

void HistoryUiTest::loadFixture() {
  auto* source = find<QComboBox>("historySource");
  source->setCurrentIndex(source->findData("coinbase"));
  find<QLineEdit>("historySymbol")->setText("BTC-USD");
  auto* interval = find<QComboBox>("historyInterval");
  interval->setCurrentIndex(interval->findText("1D"));
  QTest::mouseClick(find<QPushButton>("historyLoad"), Qt::LeftButton);
  QTRY_VERIFY_WITH_TIMEOUT(find<QPushButton>("historyLoad")->isEnabled(), 15000);
}

void HistoryUiTest::optionsFillTheControls() {
  auto* source = find<QComboBox>("historySource");
  QCOMPARE(source->count(), 2);
  QCOMPARE(source->currentData().toString(), QStringLiteral("coinbase"));
  QCOMPARE(find<QLineEdit>("historySymbol")->text(), QStringLiteral("BTC-USD"));
  QCOMPARE(find<QComboBox>("historyInterval")->currentText(), QStringLiteral("1D"));
}

void HistoryUiTest::changingSourceChangesTimeframesAndExample() {
  auto* source = find<QComboBox>("historySource");
  auto* symbol = find<QLineEdit>("historySymbol");
  auto* interval = find<QComboBox>("historyInterval");
  QVERIFY(interval->findText("1W") < 0);

  source->setCurrentIndex(source->findData("yahoo"));
  QVERIFY(interval->findText("1W") >= 0);
  QCOMPARE(symbol->text(), QStringLiteral("AAPL"));

  // Something the person typed is kept.
  symbol->setText("MSFT");
  source->setCurrentIndex(source->findData("coinbase"));
  QCOMPARE(symbol->text(), QStringLiteral("MSFT"));
  QVERIFY(interval->findText("1W") < 0);
}

void HistoryUiTest::exportIsDisabledUntilSomethingLoads() { QVERIFY(!find<QPushButton>("historyExport")->isEnabled()); }

void HistoryUiTest::loadShowsChartTableAndHonestSummary() {
  loadFixture();
  auto* chart = find<rq::CandleChart>("historyChart");
  QCOMPARE(chart->candleCount(), kFixtureBars);
  auto* table = find<QTableView>("historyTable");
  QCOMPARE(table->model()->rowCount(), kFixtureBars);

  // The table is newest first, and the chart oldest first.
  const QString newest = table->model()->index(0, 0).data().toString();
  const QString oldest = table->model()->index(kFixtureBars - 1, 0).data().toString();
  QVERIFY2(newest > oldest, qPrintable(newest + " " + oldest));
  QVERIFY2(chart->readout(kFixtureBars - 1).startsWith(newest), qPrintable(chart->readout(kFixtureBars - 1)));

  const QString status = find<QLabel>("historyStatus")->text();
  QVERIFY2(status.contains("BTC-USD 1D"), qPrintable(status));
  QVERIFY2(status.contains("39 bars"), qPrintable(status));
  QVERIFY2(status.contains("Source: Coinbase"), qPrintable(status));

  auto* warnings = find<QLabel>("historyWarnings");
  QVERIFY(warnings->isVisible());
  QVERIFY2(warnings->text().contains("Missing: 1 bar across 1 gap"), qPrintable(warnings->text()));
  QVERIFY2(warnings->text().contains("failing validation: 2 bars"), qPrintable(warnings->text()));
  QVERIFY(find<QPushButton>("historyExport")->isEnabled());
}

void HistoryUiTest::missingVolumeStaysBlankInTheTable() {
  auto* model = find<QTableView>("historyTable")->model();
  int blanks = 0;
  for (int row = 0; row < model->rowCount(); ++row) {
    if (model->index(row, 5).data().toString().isEmpty()) ++blanks;
  }
  QCOMPARE(blanks, 1);
}

void HistoryUiTest::exportWritesExactlyTheBarsShown() {
  const QString path = folder_.filePath("export.csv");
  panel_->exportTo(path);
  QTRY_VERIFY_WITH_TIMEOUT(find<QLabel>("historyStatus")->text().startsWith("Saved 39 bars"), 10000);

  QFile file(path);
  QVERIFY(file.open(QIODevice::ReadOnly));
  const QByteArray bytes = file.readAll();
  QVERIFY(bytes.contains("\r\n"));
  const QList<QByteArray> lines = bytes.trimmed().split('\n');
  QCOMPARE(lines.size(), kFixtureBars + 1);
  QVERIFY(lines[0].startsWith("time_utc,symbol,interval,open,high,low,close,volume,source,downloaded_at_utc"));

  // Each row matches the table, in the same order.
  auto* model = find<QTableView>("historyTable")->model();
  for (int row = 0; row < kFixtureBars; ++row) {
    const QList<QByteArray> fields = lines[row + 1].trimmed().split(',');
    QCOMPARE(fields[1], QByteArray("BTC-USD"));
    QVERIFY(QString::fromLatin1(fields[0]).replace('T', ' ').startsWith(model->index(row, 0).data().toString()));
    for (int column = 1; column <= 5; ++column) {
      const QString cell = model->index(row, column).data().toString();
      const QByteArray exported = fields[column + 2];
      QCOMPARE(exported.isEmpty(), cell.isEmpty());
      if (!cell.isEmpty()) QCOMPARE(exported.toDouble(), cell.toDouble());
    }
  }
  QVERIFY2(QDir(folder_.path()).entryList({"*.tmp"}, QDir::Files).isEmpty(), "A temporary file was left behind");
}

void HistoryUiTest::exportFailureIsShownAndKeepsTheBars() {
  panel_->exportTo(folder_.filePath("no-such-folder/export.csv"));
  auto* message = find<QLabel>("historyErrorMessage");
  QTRY_VERIFY_WITH_TIMEOUT(message->isVisible(), 10000);
  QVERIFY2(message->text().contains("Error"), qPrintable(message->text()));
  QVERIFY(find<QPushButton>("historyErrorToggle")->isVisible());
  QVERIFY(!find<QPlainTextEdit>("historyErrorDetails")->isVisible());
  QCOMPARE(find<rq::CandleChart>("historyChart")->candleCount(), kFixtureBars);
  QVERIFY(find<QPushButton>("historyExport")->isEnabled());
}

void HistoryUiTest::badSymbolShowsPlainMessageAndClearsOldBars() {
  find<QLineEdit>("historySymbol")->setText("not a pair");
  QTest::mouseClick(find<QPushButton>("historyLoad"), Qt::LeftButton);
  auto* message = find<QLabel>("historyErrorMessage");
  QTRY_VERIFY_WITH_TIMEOUT(message->text().startsWith("Enter a symbol"), 10000);
  QTRY_VERIFY_WITH_TIMEOUT(find<QPushButton>("historyLoad")->isEnabled(), 10000);
  QVERIFY(!find<QPushButton>("historyErrorToggle")->isVisible());  // A correctable input needs no traceback.

  QCOMPARE(find<rq::CandleChart>("historyChart")->candleCount(), 0);
  QCOMPARE(find<QTableView>("historyTable")->model()->rowCount(), 0);
  QVERIFY(!find<QPushButton>("historyExport")->isEnabled());
  QVERIFY(!find<QLabel>("historyWarnings")->isVisible());

  // A start date after the end date also comes back as a plain message.
  find<QLineEdit>("historySymbol")->setText("BTC-USD");
  auto* start = find<QDateEdit>("historyStart");
  auto* end = find<QDateEdit>("historyEnd");
  const QDate savedStart = start->date();
  start->setDate(end->date());
  end->setDate(end->date().addDays(-1));
  const QDate savedEnd = end->date().addDays(1);
  QTest::mouseClick(find<QPushButton>("historyLoad"), Qt::LeftButton);
  QTRY_VERIFY_WITH_TIMEOUT(message->text().contains("must not be after"), 10000);
  QTRY_VERIFY_WITH_TIMEOUT(find<QPushButton>("historyLoad")->isEnabled(), 10000);
  end->setDate(savedEnd);
  start->setDate(savedStart);
}

void HistoryUiTest::chartZoomAndPanStayInBounds() {
  rq::CandleChart chart;
  chart.resize(800, 400);
  chart.setCandles(makeCandles(1000), false);
  QCOMPARE(chart.firstVisible(), 0);
  QCOMPARE(chart.visibleCount(), 1000);

  chart.zoomBy(2.0);  // Anchored at the right edge, so the newest bars stay in view.
  QCOMPARE(chart.visibleCount(), 500);
  QCOMPARE(chart.firstVisible() + chart.visibleCount(), 1000);

  chart.panBy(-100);
  QCOMPARE(chart.firstVisible(), 400);
  chart.panBy(-100000);
  QCOMPARE(chart.firstVisible(), 0);
  chart.panBy(100000);
  QCOMPARE(chart.firstVisible() + chart.visibleCount(), 1000);

  for (int i = 0; i < 100; ++i) chart.zoomBy(2.0);
  QCOMPARE(chart.visibleCount(), 10);
  QCOMPARE(chart.firstVisible() + chart.visibleCount(), 1000);
  for (int i = 0; i < 100; ++i) chart.zoomBy(0.5);
  QCOMPARE(chart.visibleCount(), 1000);
  QCOMPARE(chart.firstVisible(), 0);

  // Empty and very small sets must not crash or divide by zero.
  chart.setCandles(makeCandles(3), false);
  QCOMPARE(chart.visibleCount(), 3);
  chart.zoomBy(4.0);
  QCOMPARE(chart.visibleCount(), 3);
  chart.clear();
  chart.zoomBy(2.0);
  chart.panBy(5);
  QCOMPARE(chart.indexAtX(100), -1);
  QVERIFY(!chart.grab().isNull());
}

void HistoryUiTest::chartWheelAndKeyboard() {
  rq::CandleChart chart;
  chart.resize(800, 400);
  chart.setCandles(makeCandles(1000), false);
  chart.show();
  QVERIFY(QTest::qWaitForWindowExposed(&chart));

  const QPointF middle(chart.plotRect().center());
  QWheelEvent wheel(middle, chart.mapToGlobal(middle), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
  QApplication::sendEvent(&chart, &wheel);
  QCOMPARE(chart.visibleCount(), 800);
  QVERIFY(chart.firstVisible() > 0);  // Zooms around the pointer, in the middle.
  QVERIFY(chart.firstVisible() + chart.visibleCount() < 1000);

  const int before = chart.firstVisible();
  QTest::keyClick(&chart, Qt::Key_Left);
  QCOMPARE(chart.firstVisible(), before - 80);
  QTest::keyClick(&chart, Qt::Key_Home);
  QCOMPARE(chart.visibleCount(), 1000);

  // Hovering picks the candle under the pointer, and leaving clears it.
  const QPointF point(chart.plotRect().center());
  QMouseEvent move(QEvent::MouseMove, point, chart.mapToGlobal(point), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(&chart, &move);
  QCOMPARE(chart.hoveredIndex(), chart.indexAtX(chart.plotRect().center().x()));
  QVERIFY(chart.hoveredIndex() >= 0);
  QEvent leave(QEvent::Leave);
  QApplication::sendEvent(&chart, &leave);
  QCOMPARE(chart.hoveredIndex(), -1);
}

void HistoryUiTest::chartDrawsBothCandleColours() {
  rq::CandleChart chart;
  chart.resize(800, 400);
  chart.setCandles(makeCandles(40), false);
  const QImage image = chart.grab().toImage().convertToFormat(QImage::Format_RGB32);
  QVERIFY(imageHasColor(image, qRgb(0x2b, 0xd9, 0x8a)));
  QVERIFY(imageHasColor(image, qRgb(0xff, 0x5a, 0x52)));

  chart.clear(QStringLiteral("Nothing"));
  const QImage empty = chart.grab().toImage().convertToFormat(QImage::Format_RGB32);
  QVERIFY(!imageHasColor(empty, qRgb(0x2b, 0xd9, 0x8a)));
}

void HistoryUiTest::chartHandlesFlatPricesAndNegativePrices() {
  rq::CandleChart chart;
  chart.resize(800, 400);
  QVector<rq::Candle> candles = makeCandles(20);
  for (rq::Candle& candle : candles) {
    candle.open = candle.high = candle.low = candle.close = 50.0;
    candle.hasVolume = false;  // No volume pane at all, rather than bars of zero.
  }
  chart.setCandles(candles, true);
  QVERIFY(!chart.grab().isNull());
  QVERIFY2(chart.readout(0).contains("V not reported"), qPrintable(chart.readout(0)));

  for (rq::Candle& candle : candles) {  // Oil futures have traded below zero.
    candle.open = -5.0;
    candle.close = -3.0;
    candle.high = -2.5;
    candle.low = -6.0;
  }
  chart.setCandles(candles, true);
  const QImage image = chart.grab().toImage().convertToFormat(QImage::Format_RGB32);
  QVERIFY(imageHasColor(image, qRgb(0x2b, 0xd9, 0x8a)));
}

void HistoryUiTest::cleanupTestCase() {
  panel_.reset();
  QVERIFY(engine_->shutdown());
}

QTEST_MAIN(HistoryUiTest)
#include "test_history_ui.moc"
