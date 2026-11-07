// Drives the real window in Qt's offscreen mode, with the real Python behind it.
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QTemporaryDir>
#include <QtTest>

#include "app/HealthPanel.h"
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "bridge/PythonEngine.h"

namespace {

// Set before measuring: a pause under a quarter of a second is not a frozen window.
constexpr qint64 kMaxAllowedGapMs = 250;

template <typename T>
T* find(QWidget& parent, const char* name) {
  T* widget = parent.findChild<T*>(QString::fromLatin1(name));
  if (widget == nullptr) qFatal("Widget not found: %s", name);
  return widget;
}

}  // namespace

class UiTest : public QObject {
  Q_OBJECT

 private slots:
  void initTestCase();
  void navigationSwitchesPages();
  void healthPanelShowsPythonState();
  void calculationAppearsInTheWindow();
  void pythonErrorAppearsWithDetails();
  void windowStaysLiveDuringALongJobAndCancels();
  void cleanupTestCase();

 private:
  QTemporaryDir folder_;
  std::unique_ptr<rq::PythonEngine> engine_;
  std::unique_ptr<rq::MainWindow> window_;
};

void UiTest::initTestCase() {
  qApp->setStyleSheet(rq::applicationStyleSheet());
  engine_ = std::make_unique<rq::PythonEngine>(rq::PythonEngine::locate());
  rq::TerminalOptions options;
  options.connect = false;  // These tests contact no provider.
  options.showIntro = false;
  options.settingsPath = folder_.filePath(QStringLiteral("terminal.ini"));
  window_ = std::make_unique<rq::MainWindow>(*engine_, options);
  window_->show();
  QVERIFY(QTest::qWaitForWindowExposed(window_.get()));
  QCOMPARE(find<QLabel>(*window_, "pythonState")->text(), QStringLiteral("Python: starting"));
  engine_->start();
  QTRY_COMPARE_WITH_TIMEOUT(find<QLabel>(*window_, "pythonState")->text(), QStringLiteral("Python: running"), 30000);
}

void UiTest::navigationSwitchesPages() {
  QCOMPARE(window_->currentPage(), window_->terminal());
  window_->showHistory(QStringLiteral("BTC-USD"), false);
  QVERIFY(window_->currentPage() != window_->terminal());
  window_->showTerminal();
  QCOMPARE(window_->currentPage(), window_->terminal());
  window_->showHealth();  // The Python Health page, which the tests below use.
  QVERIFY(window_->currentPage() != window_->terminal());
}

void UiTest::healthPanelShowsPythonState() {
  QTRY_COMPARE_WITH_TIMEOUT(find<QLabel>(*window_, "pythonVersion")->text(), QStringLiteral("3.13.17"), 30000);
  QCOMPARE(find<QLabel>(*window_, "numpyVersion")->text(), QStringLiteral("2.5.4"));
  QVERIFY(find<QLabel>(*window_, "pythonIsolation")->text().startsWith("Active"));

  auto* button = find<QPushButton>(*window_, "runHealthCheck");
  QVERIFY(button->isEnabled());
  QTest::mouseClick(button, Qt::LeftButton);
  QVERIFY(!button->isEnabled());
  QTRY_VERIFY_WITH_TIMEOUT(button->isEnabled(), 10000);
}

void UiTest::calculationAppearsInTheWindow() {
  auto* input = find<QLineEdit>(*window_, "calculationInput");
  input->clear();
  QTest::keyClicks(input, "2, 4, 4, 4, 5, 5, 7, 9");
  QTest::mouseClick(find<QPushButton>(*window_, "calculate"), Qt::LeftButton);
  auto* result = find<QLabel>(*window_, "calculationResult");
  QTRY_VERIFY_WITH_TIMEOUT(result->text().contains("mean 5,"), 10000);
  QVERIFY2(result->text().contains("Count 8"), qPrintable(result->text()));
  QVERIFY2(result->text().contains("2.13808993"), qPrintable(result->text()));
}

void UiTest::pythonErrorAppearsWithDetails() {
  auto* input = find<QLineEdit>(*window_, "calculationInput");
  input->setText("1, oops");
  QTest::mouseClick(find<QPushButton>(*window_, "calculate"), Qt::LeftButton);
  auto* error = find<QLabel>(*window_, "calculationErrorMessage");
  QTRY_VERIFY_WITH_TIMEOUT(error->isVisible(), 10000);
  QVERIFY2(error->text().startsWith("ValueError"), qPrintable(error->text()));
  QVERIFY(error->text().contains("oops"));

  // The traceback stays hidden until asked for.
  auto* details = find<QPlainTextEdit>(*window_, "calculationErrorDetails");
  auto* toggle = find<QPushButton>(*window_, "calculationErrorToggle");
  QVERIFY(!details->isVisible());
  QCOMPARE(toggle->text(), QStringLiteral("Show details"));
  QTest::mouseClick(toggle, Qt::LeftButton);
  QVERIFY(details->isVisible());
  QVERIFY(details->toPlainText().contains("Traceback"));
  QCOMPARE(toggle->text(), QStringLiteral("Hide details"));
  QTest::mouseClick(toggle, Qt::LeftButton);
  QVERIFY(!details->isVisible());

  input->setText("1, 2");
  QTest::mouseClick(find<QPushButton>(*window_, "calculate"), Qt::LeftButton);
  QTRY_VERIFY_WITH_TIMEOUT(!error->isVisible(), 10000);
  QVERIFY(!details->isVisible());
  QVERIFY(!toggle->isVisible());
}

void UiTest::windowStaysLiveDuringALongJobAndCancels() {
  auto* panel = window_->findChild<rq::HealthPanel*>();
  auto* start = find<QPushButton>(*window_, "longStart");
  auto* cancel = find<QPushButton>(*window_, "longCancel");
  auto* progress = find<QProgressBar>(*window_, "longProgress");
  auto* status = find<QLabel>(*window_, "longStatus");

  QTest::mouseClick(start, Qt::LeftButton);
  QVERIFY(!start->isEnabled());
  QVERIFY(cancel->isEnabled());
  QTRY_VERIFY_WITH_TIMEOUT(progress->value() >= 20, 15000);

  QTest::mouseClick(cancel, Qt::LeftButton);
  QTRY_VERIFY_WITH_TIMEOUT(status->text().startsWith("Cancelled"), 10000);
  QVERIFY(start->isEnabled());
  QVERIFY(progress->value() < 100);
  qInfo("Longest gap between window updates during the job: %lld ms", panel->longJobMaxGapMs());
  QVERIFY(panel->longJobMaxGapMs() > 0);
  QVERIFY2(panel->longJobMaxGapMs() < kMaxAllowedGapMs, "The window stalled during the job");

  // Lets a person check the look of the page: set RQ_SCREENSHOT to a .png path.
  const QString screenshot = qEnvironmentVariable("RQ_SCREENSHOT");
  if (!screenshot.isEmpty()) QVERIFY(window_->grab().save(screenshot));
}

void UiTest::cleanupTestCase() {
  window_.reset();
  QVERIFY(engine_->shutdown());
}

QTEST_MAIN(UiTest)
#include "test_ui.moc"
