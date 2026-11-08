// A manual check that contacts real providers, so it is not part of ctest. Run it yourself:
//   live_history_check [screenshot.png]
// It prints bar counts and timings only, and leaves no market data behind.
#include <QApplication>
#include <QComboBox>
#include <QDateEdit>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <cstdio>

#include "app/CandleChart.h"
#include "app/HistoryPanel.h"
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

// Returns true if the source returned bars, the chart and table agree, and the export has every row.
bool check(rq::HistoryPanel& panel, const QTemporaryDir& folder, const char* source, const char* symbol, const char* interval, int days) {
  auto* sources = find<QComboBox>(panel, "historySource");
  sources->setCurrentIndex(sources->findData(QString::fromLatin1(source)));
  find<QLineEdit>(panel, "historySymbol")->setText(QString::fromLatin1(symbol));
  auto* intervals = find<QComboBox>(panel, "historyInterval");
  intervals->setCurrentIndex(intervals->findText(QString::fromLatin1(interval)));
  const QDate today = QDateTime::currentDateTimeUtc().date();
  find<QDateEdit>(panel, "historyEnd")->setDate(today);
  find<QDateEdit>(panel, "historyStart")->setDate(today.addDays(-days));

  auto* load = find<QPushButton>(panel, "historyLoad");
  QElapsedTimer clock;
  clock.start();
  QTest::mouseClick(load, Qt::LeftButton);
  if (!waitUntil([&] { return load->isEnabled(); }, 90000)) {
    std::printf("FAIL %s %s %s: timed out\n", source, symbol, interval);
    return false;
  }
  const int bars = find<rq::CandleChart>(panel, "historyChart")->candleCount();
  const int rows = find<QTableView>(panel, "historyTable")->model()->rowCount();
  const QString warnings = find<QLabel>(panel, "historyWarnings")->isVisibleTo(&panel) ? find<QLabel>(panel, "historyWarnings")->text() : QString();
  if (bars == 0 || bars != rows) {
    const QString message = find<QLabel>(panel, "historyErrorMessage")->text();
    std::printf("FAIL %s %s %s: %d bars, %d rows. %s\n", source, symbol, interval, bars, rows, qPrintable(message));
    return false;
  }

  const QString path = folder.filePath(QStringLiteral("%1-%2.csv").arg(QString::fromLatin1(source), QString::fromLatin1(interval)));
  panel.exportTo(path);
  auto* status = find<QLabel>(panel, "historyStatus");
  if (!waitUntil([&] { return status->text().startsWith(QStringLiteral("Saved")); }, 10000)) {
    std::printf("FAIL %s %s %s: export did not finish\n", source, symbol, interval);
    return false;
  }
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    std::printf("FAIL %s %s %s: the export could not be read back\n", source, symbol, interval);
    return false;
  }
  const int exported = static_cast<int>(file.readAll().count('\n')) - 1;
  std::printf("%s %s %s %s: %d bars in %lld ms, exported %d rows. %s\n", exported == bars ? "PASS" : "FAIL", source, symbol, interval,
              bars, clock.elapsed(), exported, qPrintable(warnings));
  return exported == bars;
}

}  // namespace

int main(int argc, char* argv[]) {
  QApplication app(argc, argv);
  app.setStyleSheet(rq::applicationStyleSheet());
  rq::PythonEngine engine(rq::PythonEngine::locate());
  rq::HistoryPanel panel(engine);
  panel.resize(1200, 800);
  panel.show();
  engine.start();
  if (!waitUntil([&] { return find<QPushButton>(panel, "historyLoad")->isEnabled(); }, 30000)) {
    std::puts("FAIL Python did not start");
    return 1;
  }

  QTemporaryDir folder;
  bool ok = check(panel, folder, "yahoo", "AAPL", "1H", 30);
  ok = check(panel, folder, "coinbase", "ETH-USD", "1H", 30) && ok;
  ok = check(panel, folder, "yahoo", "^IXIC", "1D", 180) && ok;
  ok = check(panel, folder, "coinbase", "BTC-USD", "1D", 180) && ok;  // Last, for the screenshot.
  if (argc > 1) panel.grab().save(QString::fromLocal8Bit(argv[1]));

  QElapsedTimer clock;
  clock.start();
  const bool stopped = engine.shutdown();
  std::printf("%s shutdown with the provider libraries loaded took %lld ms\n", stopped ? "PASS" : "FAIL", clock.elapsed());
  std::fflush(stdout);
  return ok && stopped ? 0 : 1;
}
