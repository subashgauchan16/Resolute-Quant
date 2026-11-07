// A manual check that streams from the real Coinbase feed, so it is not part of ctest. Run it yourself:
//   live_feed_check [seconds] [screenshot.png]
// It prints counts, timings and memory only, and keeps no market data. It opens a real window, so
// clicking in it changes what is being measured. Set QT_QPA_PLATFORM=offscreen for a long run.
#include <windows.h>
// psapi.h needs windows.h first.
#include <psapi.h>

#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QTableView>
#include <QTest>
#include <QTimer>
#include <cstdio>

#include "app/CandleChart.h"
#include "app/LivePanel.h"
#include "app/Theme.h"
#include "bridge/PythonEngine.h"

namespace {

template <typename T>
T* find(QWidget& parent, const char* name) {
  return parent.findChild<T*>(QString::fromLatin1(name));
}

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
  QApplication app(argc, argv);
  app.setStyleSheet(rq::applicationStyleSheet());
  const int seconds = argc > 1 ? QString::fromLocal8Bit(argv[1]).toInt() : 30;

  rq::PythonEngine engine(rq::PythonEngine::locate());
  rq::LivePanel panel(engine);
  panel.resize(1300, 800);
  panel.show();
  engine.start();

  int events = 0;
  int tickers = 0;
  QObject::connect(&engine, &rq::PythonEngine::pythonEvent, &app, [&](const QJsonObject& message) {
    ++events;
    tickers += static_cast<int>(message["tickers"].toArray().size());
  });

  auto* start = find<QPushButton>(panel, "liveStart");
  if (!waitUntil([&] { return start->isEnabled(); }, 30000)) {
    std::puts("FAIL Python did not start");
    return 1;
  }
  QTest::mouseClick(start, Qt::LeftButton);
  QElapsedTimer clock;
  clock.start();
  if (!waitUntil([&] { return panel.isLive(); }, 30000)) {
    std::printf("FAIL did not go live: %s\n", qPrintable(find<QLabel>(panel, "liveStatus")->text()));
    return 1;
  }
  std::printf("PASS went live in %lld ms\n", clock.elapsed());

  auto* chart = find<rq::CandleChart>(panel, "liveChart");
  if (!waitUntil([&] { return chart->candleCount() > 0; }, 15000)) {
    std::puts("FAIL the chart did not load");
    return 1;
  }
  std::printf("PASS chart loaded %d bars\n", chart->candleCount());

  // Records the longest gap between window updates, and whether the feed ever stopped being live.
  qint64 largestGap = 0;
  int notLiveChecks = 0;
  QElapsedTimer gapClock;
  gapClock.start();
  QTimer gapTimer;
  QObject::connect(&gapTimer, &QTimer::timeout, &app, [&] {
    largestGap = std::max(largestGap, gapClock.restart());
    if (!panel.isLive()) ++notLiveChecks;
  });
  gapTimer.start(20);

  const double memoryStart = memoryMb();
  const int eventsAtStart = events;
  const int barsAtStart = chart->candleCount();
  const QString firstReadout = chart->readout(chart->candleCount() - 1);
  for (int elapsed = 0; elapsed < seconds; elapsed += 60) {
    QTest::qWait(std::min(seconds - elapsed, 60) * 1000);
    if (seconds > 60) {
      std::printf("INFO at %d s: %d bars, %.1f MB, longest gap so far %lld ms\n", std::min(elapsed + 60, seconds), chart->candleCount(), memoryMb(), largestGap);
      std::fflush(stdout);
    }
  }
  gapTimer.stop();

  const auto* model = find<QTableView>(panel, "liveTable")->model();
  int priced = 0;
  for (int row = 0; row < model->rowCount(); ++row) {
    if (!model->index(row, 1).data().toString().isEmpty()) ++priced;
  }
  const int messages = events - eventsAtStart;
  // On one-minute bars, a long run must add about a bar a minute. Just changing the newest bar isn't enough.
  const int expectedNewBars = std::max(seconds / 60 - 1, 0);
  const bool barMoved = chart->candleCount() - barsAtStart >= expectedNewBars && (chart->candleCount() > barsAtStart || chart->readout(chart->candleCount() - 1) != firstReadout);
  bool ok = true;
  const auto report = [&](bool pass, const QString& text) {
    std::printf("%s %s\n", pass ? "PASS" : "FAIL", qPrintable(text));
    ok = ok && pass;
  };
  report(priced == model->rowCount(), QStringLiteral("%1 of %2 pairs have a price").arg(priced).arg(model->rowCount()));
  report(barMoved, QStringLiteral("the chart's bars went from %1 to %2, needing at least %3 new").arg(barsAtStart).arg(chart->candleCount()).arg(expectedNewBars));
  report(messages <= (seconds + 1) * 6, QStringLiteral("%1 window messages in %2 s, carrying %3 price updates").arg(messages).arg(seconds).arg(tickers));
  report(largestGap < 250, QStringLiteral("longest gap between window updates was %1 ms").arg(largestGap));
  std::printf("INFO not-live checks: %d of about %d. Status: %s\n", notLiveChecks, seconds * 50, qPrintable(find<QLabel>(panel, "liveStatus")->text()));
  std::printf("INFO memory %.1f MB at start and %.1f MB at the end, over %d s\n", memoryStart, memoryMb(), seconds);

  if (argc > 2) panel.grab().save(QString::fromLocal8Bit(argv[2]));

  clock.restart();
  const bool stopped = engine.shutdown();
  report(stopped, QStringLiteral("shutdown while streaming took %1 ms").arg(clock.elapsed()));
  std::fflush(stdout);
  return ok ? 0 : 1;
}
