// A manual check that streams from real providers, so it is not part of ctest. Run it yourself:
//   live_feed_check [seconds] [screenshot.png]
// It prints counts, timings and memory only, and keeps no market data. It opens a real window, so
// clicking in it changes what is being measured. Set QT_QPA_PLATFORM=offscreen for a long run.
#include <windows.h>
// psapi.h needs windows.h first.
#include <psapi.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <cstdio>

#include "app/BrandMark.h"
#include "app/CandleChart.h"
#include "app/MainWindow.h"
#include "app/TerminalWidgets.h"
#include "app/Theme.h"
#include "bridge/PythonEngine.h"

namespace {

// Times everything the window thread does, to tell slow app code from Windows not scheduling the app.
class TimingApplication : public QApplication {
 public:
  using QApplication::QApplication;

  bool notify(QObject* receiver, QEvent* event) override {
    if (depth_ > 0) return QApplication::notify(receiver, event);
    ++depth_;
    QElapsedTimer clock;
    clock.start();
    const int type = event->type();
    const QString name = QString::fromLatin1(receiver->metaObject()->className());
    const bool result = QApplication::notify(receiver, event);
    const qint64 elapsed = clock.elapsed();
    busyMs += elapsed;
    if (elapsed > slowestMs) {
      slowestMs = elapsed;
      slowestWhat = QStringLiteral("%1, event type %2").arg(name).arg(type);
    }
    if (elapsed >= 50) ++slowEvents;
    --depth_;
    return result;
  }

  qint64 busyMs = 0;
  qint64 slowestMs = 0;
  int slowEvents = 0;
  QString slowestWhat;

 private:
  int depth_ = 0;
};

bool waitUntil(const std::function<bool()>& condition, int timeoutMs) {
  QElapsedTimer clock;
  clock.start();
  while (clock.elapsed() < timeoutMs) {
    if (condition()) return true;
    QTest::qWait(50);
  }
  return condition();
}

double memoryMb() {
  PROCESS_MEMORY_COUNTERS counters{};
  GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters));
  return static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0);
}

}  // namespace

int main(int argc, char* argv[]) {
  TimingApplication app(argc, argv);
  rq::loadFonts();
  app.setStyleSheet(rq::applicationStyleSheet());
  const int seconds = argc > 1 ? QString::fromLocal8Bit(argv[1]).toInt() : 30;

  QTemporaryDir folder;
  rq::PythonEngine engine(rq::PythonEngine::locate());
  rq::TerminalOptions options;
  options.showIntro = false;
  options.settingsPath = folder.filePath(QStringLiteral("terminal.ini"));
  rq::MainWindow window(engine, options);
  window.resize(1500, 900);
  window.show();
  auto* terminal = window.terminal();
  terminal->setTimeframe(QStringLiteral("1M"));
  engine.start();

  int events = 0;
  QObject::connect(&engine, &rq::PythonEngine::pythonEvent, &app, [&](const QJsonObject&) { ++events; });

  QElapsedTimer clock;
  clock.start();
  if (!waitUntil([&] { return terminal->isLive(); }, 30000)) {
    std::puts("FAIL did not go live");
    return 1;
  }
  std::printf("PASS went live in %lld ms\n", clock.elapsed());

  auto* chart = window.findChild<rq::CandleChart*>(QStringLiteral("chart"));
  auto* book = window.findChild<rq::OrderBookView*>(QStringLiteral("orderBook"));
  auto* tape = window.findChild<rq::TapeView*>(QStringLiteral("tape"));
  auto* watchlist = window.findChild<rq::WatchlistView*>();
  if (!waitUntil([&] { return chart->candleCount() > 0; }, 20000)) {
    std::puts("FAIL the chart did not load");
    return 1;
  }
  std::printf("PASS chart loaded %d bars\n", chart->candleCount());

  qint64 largestGap = 0;
  qint64 startupGap = 0;
  int notLiveChecks = 0;
  QElapsedTimer gapClock;
  gapClock.start();
  QTimer gapTimer;
  QElapsedTimer runClock;
  runClock.start();
  app.busyMs = 0;
  app.slowestMs = 0;
  app.slowEvents = 0;
  QObject::connect(&gapTimer, &QTimer::timeout, &app, [&] {
    const qint64 gap = gapClock.restart();
    // The window's first full draw, as the data arrives, is reported but held to no limit.
    if (runClock.elapsed() < 5000) {
      startupGap = std::max(startupGap, gap);
    } else {
      largestGap = std::max(largestGap, gap);
    }
    if (gap >= 100) {
      std::printf("INFO a gap of %lld ms at %.1f s; slowest window event so far %lld ms (%s)\n", gap, runClock.elapsed() / 1000.0, app.slowestMs, qPrintable(app.slowestWhat));
      std::fflush(stdout);
    }
    if (!terminal->isLive()) ++notLiveChecks;
  });
  gapTimer.start(20);

  const double memoryStart = memoryMb();
  const int eventsAtStart = events;
  const int barsAtStart = chart->candleCount();
  for (int elapsed = 0; elapsed < seconds; elapsed += 60) {
    QTest::qWait(std::min(seconds - elapsed, 60) * 1000);
    if (seconds > 60) {
      std::printf("INFO at %d s: %d bars, %.1f MB, longest gap so far %lld ms\n", std::min(elapsed + 60, seconds), chart->candleCount(), memoryMb(), largestGap);
      std::fflush(stdout);
    }
  }
  gapTimer.stop();

  int pricedCrypto = 0;
  int pricedMarkets = 0;
  int sparked = 0;
  for (const QString& symbol : terminal->cryptoSymbols()) {
    const auto* row = watchlist->row(symbol);
    pricedCrypto += row->price != QStringLiteral("-");
    sparked += row->spark.size() >= 2;
  }
  for (const QString& symbol : terminal->marketSymbols()) {
    const auto* row = watchlist->row(symbol);
    pricedMarkets += row->price != QStringLiteral("-");
    sparked += row->spark.size() >= 2;
  }

  bool ok = true;
  const auto report = [&](bool pass, const QString& text) {
    std::printf("%s %s\n", pass ? "PASS" : "FAIL", qPrintable(text));
    ok = ok && pass;
  };
  report(pricedCrypto == terminal->cryptoSymbols().size(), QStringLiteral("%1 of %2 crypto rows have a price").arg(pricedCrypto).arg(terminal->cryptoSymbols().size()));
  report(pricedMarkets > 0, QStringLiteral("%1 of %2 market rows have a price").arg(pricedMarkets).arg(terminal->marketSymbols().size()));
  std::printf("INFO %d rows have a sparkline\n", sparked);
  const int expectedNewBars = std::max(seconds / 60 - 1, 0);
  report(chart->candleCount() - barsAtStart >= expectedNewBars, QStringLiteral("the chart's bars went from %1 to %2, needing at least %3 new").arg(barsAtStart).arg(chart->candleCount()).arg(expectedNewBars));
  report(book->midText() != QStringLiteral("-"), QStringLiteral("the order book has a mid price"));
  report(tape->rowCount() > 0, QStringLiteral("the trade tape has %1 rows").arg(tape->rowCount()));
  const int messages = events - eventsAtStart;
  std::printf("INFO %d messages from Python in %d s\n", messages, seconds);
  std::printf("INFO longest gap in the first 5 s, while the window first draws: %lld ms\n", startupGap);
  report(largestGap < 250, QStringLiteral("longest gap between window updates after that was %1 ms").arg(largestGap));
  std::printf("INFO not-live checks: %d of about %d\n", notLiveChecks, seconds * 50);
  std::printf("INFO window thread busy %.1f%% of the time. Slowest event %lld ms (%s). Events of 50 ms or more: %d\n", app.busyMs / 10.0 / std::max(runClock.elapsed() / 1000, 1LL), app.slowestMs, qPrintable(app.slowestWhat), app.slowEvents);
  std::printf("INFO memory %.1f MB at start and %.1f MB at the end, over %d s\n", memoryStart, memoryMb(), seconds);

  if (argc > 2) {
    const QString path = QString::fromLocal8Bit(argv[2]);
    window.grab().save(path);
    // The other views, to compare by eye. These are for this PC only, as they show real market data.
    terminal->select(QStringLiteral("AAPL"));
    waitUntil([&] { return chart->candleCount() > 0 && window.findChild<QLabel*>(QStringLiteral("news"))->text().size() > 0; }, 20000);
    QTest::qWait(1000);
    window.grab().save(QString(path).replace(QStringLiteral(".png"), QStringLiteral("-market.png")));
    terminal->intro()->open(false);
    QTest::qWait(1800);
    std::printf("INFO intro open=%d visible=%d size %dx%d\n", terminal->intro()->isOpen(), terminal->intro()->isVisible(), terminal->intro()->width(), terminal->intro()->height());
    terminal->intro()->grab().save(QString(path).replace(QStringLiteral(".png"), QStringLiteral("-intro.png")));
    window.grab().save(QString(path).replace(QStringLiteral(".png"), QStringLiteral("-intro-window.png")));
    terminal->intro()->close();
    QTest::qWait(600);
    window.showHistory(QStringLiteral("AAPL"), true);
    QTest::qWait(300);
    window.grab().save(QString(path).replace(QStringLiteral(".png"), QStringLiteral("-history.png")));
    window.showTerminal();
    terminal->select(QStringLiteral("BTC-USD"));
  }

  clock.restart();
  const bool stopped = engine.shutdown();
  report(stopped, QStringLiteral("shutdown while streaming took %1 ms").arg(clock.elapsed()));
  std::fflush(stdout);
  return ok ? 0 : 1;
}
