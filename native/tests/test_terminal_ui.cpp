// Drives the terminal with the real Python and synthetic data. No provider is contacted.
#include <QApplication>
#include <QFontDatabase>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "app/BrandMark.h"
#include "app/CandleChart.h"
#include "app/Format.h"
#include "app/MainWindow.h"
#include "app/Palette.h"
#include "app/TerminalWidgets.h"
#include "app/Theme.h"
#include "bridge/PythonEngine.h"

using namespace rq;

namespace {

constexpr qint64 kMaxAllowedGapMs = 250;  // Set before measuring, as on the Python Health page.

Candle candleAt(qint64 time, double close = 11.0) {
  Candle candle;
  candle.time = time;
  candle.open = 10.0;
  candle.high = 12.0;
  candle.low = 9.0;
  candle.close = close;
  candle.volume = 5.0;
  candle.hasVolume = true;
  return candle;
}

bool imageHasColour(const QImage& image, const QColor& colour) {
  const QRgb wanted = colour.rgb();
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      if (image.pixel(x, y) == wanted) return true;
    }
  }
  return false;
}

}  // namespace

class TerminalUiTest : public QObject {
  Q_OBJECT

 private slots:
  void initTestCase();
  void formattingMatchesTheBrowserTerminal();
  void fontsAndLogosAreBundled();
  void layoutMatchesTheBrowserGrid();
  void goesLiveByItselfWithPricesBookTapeAndStats();
  void marketRowsFillIn();
  void windowStaysResponsiveWhileStreaming();
  void selectingAMarketSwapsPanelsAndStats();
  void selectingCryptoSwapsBack();
  void timeframeButtonsAndCommandsChangeTheChart();
  void commandsAddOpenAndRemoveSymbols();
  void refusedSymbolsShowAMessage();
  void alertsDrawFireAndClear();
  void reconnectReloadsTheChart();
  void historyAndHealthOpenAndClose();
  void helpOpensAndCloses();
  void logoIntroOpensAndCloses();
  void settingsSurviveARestart();
  void chartTerminalStyle();
  void chartUpdateLatestRules();
  void offlineModeContactsNothing();
  void shutdownWhileStreamingIsClean();

 private:
  template <typename T>
  T* find(const char* name) {
    T* widget = window_->findChild<T*>(QString::fromLatin1(name));
    if (widget == nullptr) qFatal("Widget not found: %s", name);
    return widget;
  }
  QString settingsFile() const { return folder_.filePath(QStringLiteral("terminal.ini")); }
  void command(const char* text) { window_->terminal()->runCommand(QString::fromLatin1(text)); }
  quint64 restCalls();

  QTemporaryDir folder_;
  std::unique_ptr<PythonEngine> engine_;
  std::unique_ptr<MainWindow> window_;
};

quint64 TerminalUiTest::restCalls() {
  QSignalSpy finished(engine_.get(), &PythonEngine::jobFinished);
  const quint64 id = engine_->submit(QStringLiteral("_live_rest_calls_for_test"));
  for (int i = 0; i < 200; ++i) {
    for (const auto& entry : finished) {
      const auto result = entry.at(0).value<JobResult>();
      if (result.id == id) return result.value["calls"].toInteger();
    }
    finished.wait(25);
  }
  return 0;
}

void TerminalUiTest::initTestCase() {
  loadFonts();
  qApp->setStyleSheet(applicationStyleSheet());
  engine_ = std::make_unique<PythonEngine>(PythonEngine::locate());
  TerminalOptions options;
  options.startJob = QStringLiteral("_terminal_start_fixture_for_test");
  options.settingsPath = settingsFile();
  options.showIntro = false;
  window_ = std::make_unique<MainWindow>(*engine_, options);
  window_->resize(1500, 900);
  window_->show();
  QVERIFY(QTest::qWaitForWindowExposed(window_.get()));
  engine_->start();
}

void TerminalUiTest::formattingMatchesTheBrowserTerminal() {
  // Expected values worked out from helper.js, with en-GB grouping.
  QCOMPARE(format::price(75788.97), QStringLiteral("75,788.97"));
  QCOMPARE(format::price(1.1771), QStringLiteral("1.1771"));
  QCOMPARE(format::price(0.0722), QStringLiteral("0.07220"));
  QCOMPARE(format::price(0.001234), QStringLiteral("0.0012340"));
  QCOMPARE(format::price(std::nullopt), QStringLiteral("-"));
  QCOMPARE(format::size(70709.0), QStringLiteral("70,709"));
  QCOMPARE(format::size(21.02), QStringLiteral("21.02"));
  QCOMPARE(format::size(2.9857), QStringLiteral("2.9857"));
  QCOMPARE(format::size(0.005), QStringLiteral("0.005000"));
  QCOMPARE(format::compact(85840000.0), QStringLiteral("85.84M"));
  QCOMPARE(format::compact(1500.0), QStringLiteral("1.5K"));
  QCOMPARE(format::compact(2.5e12), QStringLiteral("2.50T"));
  QCOMPARE(format::percentArrow(-2.36), QStringLiteral("\u25BC 2.36%"));
  QCOMPARE(format::percentArrow(0.27), QStringLiteral("\u25B2 0.27%"));
  QCOMPARE(format::percentArrow(0.0), QStringLiteral("0.00%"));
  QCOMPARE(format::percentArrow(std::nullopt), QStringLiteral("-"));
  QCOMPARE(format::symbolPrice(true, QStringLiteral("AAPL"), 342.37), QStringLiteral("342.37"));
  QCOMPARE(format::symbolPrice(true, QStringLiteral("GBPUSD=X"), 1.3356), QStringLiteral("1.3356"));
  QCOMPARE(format::friendlyName(QStringLiteral("^GSPC")), QStringLiteral("S&P 500"));
  QCOMPARE(format::timeframeLabel(QStringLiteral("5M")), QStringLiteral("5m"));
  QCOMPARE(format::timeframeLabel(QStringLiteral("1MO")), QStringLiteral("1MO"));
  QCOMPARE(format::timeframeLabel(QStringLiteral("1H")), QStringLiteral("1H"));
  QCOMPARE(format::marketState(QStringLiteral("POST")), QStringLiteral("AFTER HOURS"));
}

void TerminalUiTest::fontsAndLogosAreBundled() {
  QVERIFY(QFontInfo(palette::mono(13)).family().contains(QStringLiteral("Plex Mono")) || QFontDatabase::families().contains(QStringLiteral("IBM Plex Mono")));
  QVERIFY(QFontDatabase::families().contains(QStringLiteral("IBM Plex Sans Condensed")));
  QCOMPARE(QPixmap(QStringLiteral(":/native/assets/img/rq-mark.png")).size(), QSize(344, 212));
  QCOMPARE(QPixmap(QStringLiteral(":/native/assets/img/resolute-quant-logo.png")).size(), QSize(1672, 941));
}

void TerminalUiTest::layoutMatchesTheBrowserGrid() {
  auto* terminal = window_->terminal();
  auto* watchlist = terminal->findChild<WatchlistView*>();
  QCOMPARE(find<QWidget>("brand")->parentWidget()->height(), 42);
  QVERIFY(watchlist->parentWidget()->parentWidget()->parentWidget()->width() == 292);
  QCOMPARE(find<QWidget>("side")->width(), 296);
  QCOMPARE(find<StatsStrip>("stats")->height(), 84);
  QCOMPARE(find<OrderBookView>("orderBook")->height(), (18 * 10 + 4) * 2 + 24);
}

void TerminalUiTest::goesLiveByItselfWithPricesBookTapeAndStats() {
  auto* terminal = window_->terminal();
  QTRY_VERIFY_WITH_TIMEOUT(terminal->isLive(), 15000);
  QTRY_COMPARE_WITH_TIMEOUT(find<QLabel>("cryptoStatus")->text(), QStringLiteral("LIVE"), 5000);
  QCOMPARE(find<QLabel>("pair")->text(), QStringLiteral("BTC-USD"));
  QTRY_VERIFY_WITH_TIMEOUT(find<QLabel>("bigPrice")->text().startsWith(QLatin1Char('$')), 5000);
  QTRY_VERIFY_WITH_TIMEOUT(find<CandleChart>("chart")->candleCount() > 0, 5000);

  auto* watchlist = terminal->findChild<WatchlistView*>();
  const WatchlistView::Row* row = watchlist->row(QStringLiteral("BTC-USD"));
  QVERIFY(row != nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(row->price != QStringLiteral("-"), 5000);
  QCOMPARE(row->title, QStringLiteral("BTC"));
  QCOMPARE(row->tag, QStringLiteral("USD"));
  QTRY_VERIFY_WITH_TIMEOUT(row->spark.size() >= 2, 5000);

  auto* book = find<OrderBookView>("orderBook");
  QTRY_VERIFY_WITH_TIMEOUT(book->midText() != QStringLiteral("-"), 5000);
  QVERIFY(book->spreadText().contains(QStringLiteral("bp")));
  QVERIFY(book->imbalance().has_value());
  QTRY_VERIFY_WITH_TIMEOUT(find<PanelHead>("bookHead")->note().startsWith(QStringLiteral("LIVE, DEPTH 40")), 5000);
  QTRY_VERIFY_WITH_TIMEOUT(find<TapeView>("tape")->rowCount() > 0, 5000);

  auto* stats = find<StatsStrip>("stats");
  const auto cell = [&](int index) { return stats->cells()[index]; };
  QCOMPARE(cell(0).label, QStringLiteral("24H RANGE"));
  QTRY_VERIFY_WITH_TIMEOUT(cell(0).value.contains(QStringLiteral(" to ")), 5000);
  QTRY_VERIFY_WITH_TIMEOUT(cell(1).value.startsWith(QLatin1Char('$')), 5000);
  QTRY_VERIFY_WITH_TIMEOUT(cell(2).value != QStringLiteral("-"), 5000);
  QTRY_VERIFY_WITH_TIMEOUT(cell(3).value.contains(QLatin1Char('/')), 5000);
  QVERIFY(cell(4).note.contains(QStringLiteral("trades since")));
  QTRY_VERIFY_WITH_TIMEOUT(cell(5).value.endsWith(QLatin1Char('%')), 5000);
  QTRY_COMPARE_WITH_TIMEOUT(cell(6).value, QStringLiteral("42 / 100"), 5000);
  QCOMPARE(cell(6).note, QStringLiteral("Fear, daily"));
  QVERIFY(find<QLabel>("legend")->text().contains(QStringLiteral("EMA 20")));
  QVERIFY(find<QLabel>("clock")->text().contains(QLatin1Char(':')));
}

void TerminalUiTest::marketRowsFillIn() {
  auto* watchlist = window_->terminal()->findChild<WatchlistView*>();
  const WatchlistView::Row* row = watchlist->row(QStringLiteral("^GSPC"));
  QVERIFY(row != nullptr);
  QCOMPARE(row->title, QStringLiteral("S&P 500"));
  QTRY_COMPARE_WITH_TIMEOUT(row->price, QStringLiteral("200.00"), 10000);
  QTRY_COMPARE_WITH_TIMEOUT(row->tag, QStringLiteral("OPEN 15M"), 10000);
  QVERIFY(row->change.startsWith(QStringLiteral("\u25B2")));
  QTRY_VERIFY_WITH_TIMEOUT(find<QLabel>("yahooStatus")->text() == QStringLiteral("YAHOO 90S"), 5000);
}

void TerminalUiTest::windowStaysResponsiveWhileStreaming() {
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

void TerminalUiTest::selectingAMarketSwapsPanelsAndStats() {
  auto* terminal = window_->terminal();
  auto* watchlist = terminal->findChild<WatchlistView*>();
  auto* chart = find<CandleChart>("chart");
  QTest::mouseClick(watchlist, Qt::LeftButton, Qt::NoModifier, watchlist->rowCentre(QStringLiteral("AAPL")));
  QCOMPARE(terminal->selected(), QStringLiteral("AAPL"));
  QCOMPARE(chart->candleCount(), 0);  // Cleared at once, so old bars never sit under the new name.
  QCOMPARE(find<QLabel>("pair")->text(), QStringLiteral("AAPL"));
  QVERIFY(!find<OrderBookView>("orderBook")->isVisible());
  QVERIFY(!find<TapeView>("tape")->isVisible());
  QTRY_VERIFY_WITH_TIMEOUT(find<QLabel>("profile")->text().contains(QStringLiteral("Test Company")), 5000);
  QVERIFY(find<QLabel>("profile")->isVisible());
  QTRY_VERIFY_WITH_TIMEOUT(find<QLabel>("news")->text().contains(QStringLiteral("Test &lt;story&gt;")), 5000);
  QTRY_VERIFY_WITH_TIMEOUT(chart->candleCount() > 0, 5000);
  QTRY_VERIFY_WITH_TIMEOUT(find<QLabel>("bigChange")->text().endsWith(QStringLiteral("TODAY")), 5000);
  QTRY_VERIFY_WITH_TIMEOUT(find<QLabel>("asOf")->text().contains(QStringLiteral("as of")), 5000);

  auto* stats = find<StatsStrip>("stats");
  QTRY_COMPARE_WITH_TIMEOUT(stats->cells()[0].label, QStringLiteral("DAY RANGE"), 5000);
  QTRY_COMPARE_WITH_TIMEOUT(stats->cells()[2].value, QStringLiteral("190.00"), 5000);
  QTRY_COMPARE_WITH_TIMEOUT(stats->cells()[5].value, QStringLiteral("20.0"), 5000);
  QCOMPARE(stats->cells()[4].value, QStringLiteral("5.00B"));
  QCOMPARE(stats->cells()[6].value, QStringLiteral("OPEN"));
}

void TerminalUiTest::selectingCryptoSwapsBack() {
  window_->terminal()->select(QStringLiteral("ETH-USD"));
  QVERIFY(find<OrderBookView>("orderBook")->isVisible());
  QVERIFY(!find<QLabel>("profile")->isVisible());
  QCOMPARE(find<QLabel>("pair")->text(), QStringLiteral("ETH-USD"));
  QTRY_VERIFY_WITH_TIMEOUT(find<OrderBookView>("orderBook")->midText() != QStringLiteral("-"), 5000);
  QTRY_VERIFY_WITH_TIMEOUT(find<CandleChart>("chart")->candleCount() > 0, 5000);
  QTRY_COMPARE_WITH_TIMEOUT(find<StatsStrip>("stats")->cells()[0].label, QStringLiteral("24H RANGE"), 5000);
}

void TerminalUiTest::timeframeButtonsAndCommandsChangeTheChart() {
  auto* terminal = window_->terminal();
  auto* chart = find<CandleChart>("chart");
  QPushButton* button = nullptr;
  for (auto* candidate : window_->findChildren<QPushButton*>(QStringLiteral("interval"))) {
    if (candidate->text() == QStringLiteral("1H")) button = candidate;
  }
  QVERIFY(button != nullptr);
  QTest::mouseClick(button, Qt::LeftButton);
  QCOMPARE(terminal->timeframe(), QStringLiteral("1H"));
  QCOMPARE(chart->candleCount(), 0);
  QTRY_VERIFY_WITH_TIMEOUT(chart->candleCount() > 0, 5000);

  command("15m");
  QCOMPARE(terminal->timeframe(), QStringLiteral("15M"));
  QTRY_VERIFY_WITH_TIMEOUT(chart->candleCount() > 0, 5000);

  command("5M");
  QTRY_VERIFY_WITH_TIMEOUT(chart->candleCount() > 0, 5000);
}

void TerminalUiTest::commandsAddOpenAndRemoveSymbols() {
  auto* terminal = window_->terminal();
  command("ADD PEPE");
  QTRY_COMPARE_WITH_TIMEOUT(terminal->selected(), QStringLiteral("PEPE-USD"), 5000);
  QVERIFY(terminal->cryptoSymbols().contains(QStringLiteral("PEPE-USD")));
  QTRY_VERIFY_WITH_TIMEOUT(terminal->findChild<WatchlistView*>()->row(QStringLiteral("PEPE-USD"))->price != QStringLiteral("-"), 5000);

  command("STK META");
  QTRY_COMPARE_WITH_TIMEOUT(terminal->selected(), QStringLiteral("META"), 5000);
  QVERIFY(terminal->marketSymbols().contains(QStringLiteral("META")));

  command("DEL META");
  QVERIFY(!terminal->marketSymbols().contains(QStringLiteral("META")));
  QVERIFY(terminal->selected() != QStringLiteral("META"));

  command("DEL PEPE");
  QVERIFY(!terminal->cryptoSymbols().contains(QStringLiteral("PEPE-USD")));

  command("BTC");
  QCOMPARE(terminal->selected(), QStringLiteral("BTC-USD"));
  command("NASDAQ");
  QCOMPARE(terminal->selected(), QStringLiteral("^IXIC"));
  command("BTC");
  QTRY_VERIFY_WITH_TIMEOUT(find<CandleChart>("chart")->candleCount() > 0, 5000);
}

void TerminalUiTest::refusedSymbolsShowAMessage() {
  auto* field = find<QLineEdit>("command");
  command("DEL NOTLISTED");
  QVERIFY(field->placeholderText().contains(QStringLiteral("is not on your watchlist")));
  command("STK NOPE");
  QTRY_VERIFY_WITH_TIMEOUT(field->placeholderText().contains(QStringLiteral("Yahoo has no price for NOPE")), 5000);
  command("ALERT");
  QVERIFY(field->placeholderText().contains(QStringLiteral("Give a price")));
  QTRY_VERIFY_WITH_TIMEOUT(field->placeholderText().startsWith(QStringLiteral("Type BTC")), 6000);
}

void TerminalUiTest::alertsDrawFireAndClear() {
  auto* terminal = window_->terminal();
  auto* bar = find<QWidget>("alertBar");
  QVERIFY(!bar->isVisible());
  // The synthetic feed cycles between 100 and 106, so 103.5 is crossed quickly.
  command("ALERT 103.5");
  QVERIFY(find<QLineEdit>("command")->placeholderText().contains(QStringLiteral("Alert set on BTC-USD")));
  QTRY_VERIFY_WITH_TIMEOUT(bar->isVisible(), 5000);
  QVERIFY2(find<QLabel>("alertText")->text().startsWith(QStringLiteral("BTC-USD crossed 103.50")), qPrintable(find<QLabel>("alertText")->text()));
  QVERIFY(QSettings(settingsFile(), QSettings::IniFormat).value(QStringLiteral("alerts")).toStringList().isEmpty());

  command("ALERT 999");
  QCOMPARE(QSettings(settingsFile(), QSettings::IniFormat).value(QStringLiteral("alerts")).toStringList().size(), 1);
  command("CLEAR");
  QVERIFY(QSettings(settingsFile(), QSettings::IniFormat).value(QStringLiteral("alerts")).toStringList().isEmpty());
  QVERIFY(terminal->isLive());
}

void TerminalUiTest::reconnectReloadsTheChart() {
  const quint64 before = restCalls();
  QVERIFY(before > 0);
  engine_->submit(QStringLiteral("_live_drop_fixture_for_test"));
  QTRY_VERIFY_WITH_TIMEOUT(restCalls() > before, 5000);
  QTRY_VERIFY_WITH_TIMEOUT(window_->terminal()->isLive(), 5000);
  QTRY_COMPARE_WITH_TIMEOUT(find<QLabel>("cryptoStatus")->text(), QStringLiteral("LIVE"), 5000);
}

void TerminalUiTest::historyAndHealthOpenAndClose() {
  auto* terminal = window_->terminal();
  QTest::mouseClick(find<QPushButton>("historyButton"), Qt::LeftButton);
  QVERIFY(window_->currentPage() != terminal);
  QCOMPARE(find<QLineEdit>("historySymbol")->text(), terminal->selected());
  QTest::mouseClick(find<QPushButton>("historyClose"), Qt::LeftButton);
  QCOMPARE(window_->currentPage(), terminal);

  command("HEALTH");
  QVERIFY(window_->currentPage() != terminal);
  QTRY_COMPARE_WITH_TIMEOUT(find<QLabel>("pythonVersion")->text(), QStringLiteral("3.13.17"), 5000);
  QTest::mouseClick(find<QPushButton>("healthClose"), Qt::LeftButton);
  QCOMPARE(window_->currentPage(), terminal);
}

void TerminalUiTest::helpOpensAndCloses() {
  QTest::mouseClick(find<QPushButton>("helpButton"), Qt::LeftButton);
  auto* help = find<QWidget>("help");
  QVERIFY(help->isVisible());
  QTest::mouseClick(help, Qt::LeftButton, Qt::NoModifier, QPoint(5, 5));
  QVERIFY(!help->isVisible());
}

void TerminalUiTest::logoIntroOpensAndCloses() {
  auto* brand = find<BrandMark>("brand");
  auto* intro = find<LogoIntro>("logoIntro");
  QVERIFY(!intro->isVisible());
  QTest::mouseClick(brand, Qt::LeftButton);
  QVERIFY(intro->isOpen());
  QTest::qWait(200);  // Lets it animate, which paints every layer.
  // It covers the whole window, including the top bar, and the logo's gold is on show.
  QCOMPARE(intro->geometry(), window_->terminal()->rect());
  const QImage frame = window_->grab().toImage().convertToFormat(QImage::Format_RGB32);
  const qreal ratio = frame.devicePixelRatio();
  const QRgb overCommandBox = frame.pixel(static_cast<int>(window_->width() * 0.5 * ratio), static_cast<int>(20 * ratio));
  QVERIFY2(qRed(overCommandBox) < 40 && qGreen(overCommandBox) < 40, "The intro did not cover the top bar");
  int goldPixels = 0;
  for (int y = 0; y < frame.height(); y += 3) {
    for (int x = 0; x < frame.width(); x += 3) {
      const QRgb pixel = frame.pixel(x, y);
      if (qRed(pixel) > 190 && qGreen(pixel) > 140 && qBlue(pixel) < 130) ++goldPixels;
    }
  }
  QVERIFY2(goldPixels > 200, qPrintable(QStringLiteral("Only %1 gold pixels in the intro").arg(goldPixels)));

  // No shine may colour the logo's dark surroundings red, which Qt's dodge blend used to do.
  int redSpeckles = 0;
  for (int y = 0; y < frame.height(); y += 2) {
    for (int x = 0; x < frame.width(); x += 2) {
      const QRgb pixel = frame.pixel(x, y);
      if (qRed(pixel) > 60 && qGreen(pixel) < 12 && qBlue(pixel) < 12) ++redSpeckles;
    }
  }
  QCOMPARE(redSpeckles, 0);
  QTest::mouseClick(intro, Qt::LeftButton);
  QTRY_VERIFY_WITH_TIMEOUT(!intro->isVisible(), 2000);
  QVERIFY(!brand->grab().isNull());

  // A key closes it too.
  intro->open(false);
  QTest::keyClick(intro, Qt::Key_Space);
  QTRY_VERIFY_WITH_TIMEOUT(!intro->isVisible(), 2000);

  // When opened at startup, it closes by itself.
  intro->open(true);
  QTRY_VERIFY_WITH_TIMEOUT(!intro->isVisible(), 6000);
}

void TerminalUiTest::settingsSurviveARestart() {
  command("5M");
  QSettings saved(settingsFile(), QSettings::IniFormat);
  QCOMPARE(saved.value(QStringLiteral("selected")).toString(), window_->terminal()->selected());
  QCOMPARE(saved.value(QStringLiteral("granularity")).toInt(), 300);
  QCOMPARE(saved.value(QStringLiteral("crypto")).toStringList(), window_->terminal()->cryptoSymbols());
}

void TerminalUiTest::chartTerminalStyle() {
  CandleChart chart;
  chart.setTerminalStyle(true);
  chart.resize(800, 500);
  QVector<Candle> candles;
  for (int i = 0; i < 300; ++i) {
    Candle candle = candleAt(1700000000 + i * 60, i % 2 == 0 ? 11.0 : 9.5);
    candles.push_back(candle);
  }
  chart.setCandles(candles, true);
  // 8 pixels a bar, with room for six empty bars on the right, as the browser chart opens.
  QCOMPARE(chart.visibleCount(), chart.plotRect().width() / 8 - 6);
  QCOMPARE(chart.firstVisible() + chart.visibleCount(), 300);
  chart.setAlertLevels({10.5});
  const QImage image = chart.grab().toImage().convertToFormat(QImage::Format_RGB32);
  QVERIFY(imageHasColour(image, palette::kUp));
  QVERIFY(imageHasColour(image, palette::kDown));
  QVERIFY(imageHasColour(image, palette::kAmber));  // EMA, last price and alert lines.

  // The EMA matches an independent calculation.
  double ema = candles[0].close;
  for (int i = 1; i < candles.size(); ++i) ema = candles[i].close * (2.0 / 21.0) + ema * (19.0 / 21.0);
  QVERIFY(std::abs(chart.emaAt(299) - ema) < 1e-9);

  // A reload keeps a view the person chose, and otherwise returns to the default.
  chart.zoomBy(2.0);
  const int zoomed = chart.visibleCount();
  chart.setCandles(candles, true, true);
  QCOMPARE(chart.visibleCount(), zoomed);
  chart.setCandles(candles, true, false);
  QVERIFY(chart.visibleCount() != zoomed);

  chart.setMessage(QStringLiteral("A note"));
  QCOMPARE(chart.message(), QStringLiteral("A note"));
  QVERIFY(!chart.grab().isNull());
}

void TerminalUiTest::chartUpdateLatestRules() {
  CandleChart chart;
  chart.resize(800, 400);
  chart.updateLatest(candleAt(100));  // Nothing loaded yet, so nothing to extend.
  QCOMPARE(chart.candleCount(), 0);

  QVector<Candle> candles;
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

void TerminalUiTest::offlineModeContactsNothing() {
  PythonEngine::Paths paths = PythonEngine::locate();
  paths.runtimeDir = QStringLiteral("C:/rq-no-such-runtime");
  PythonEngine engine(paths);
  TerminalOptions options;
  options.connect = false;
  options.showIntro = false;
  options.settingsPath = folder_.filePath(QStringLiteral("offline.ini"));
  MainWindow window(engine, options);
  auto* terminal = window.terminal();
  QVERIFY(!terminal->isLive());
  QCOMPARE(window.findChild<QLabel*>(QStringLiteral("cryptoStatus"))->text(), QStringLiteral("CRYPTO OFF"));
  QCOMPARE(window.findChild<QLabel*>(QStringLiteral("yahooStatus"))->text(), QStringLiteral("STOCKS OFF"));
  QCOMPARE(terminal->cryptoSymbols().size(), 10);
  QCOMPARE(terminal->marketSymbols().size(), 10);
  terminal->runCommand(QStringLiteral("ADD PEPE"));
  QVERIFY(window.findChild<QLineEdit*>(QStringLiteral("command"))->placeholderText().contains(QStringLiteral("Offline")));
}

void TerminalUiTest::shutdownWhileStreamingIsClean() {
  QVERIFY(window_->terminal()->isLive());
  QElapsedTimer clock;
  clock.start();
  QVERIFY(engine_->shutdown());
  qInfo("Shutdown while streaming took %lld ms", clock.elapsed());
  QVERIFY(clock.elapsed() < PythonEngine::kShutdownGraceMs);
  window_.reset();
}

QTEST_MAIN(TerminalUiTest)
#include "test_terminal_ui.moc"
