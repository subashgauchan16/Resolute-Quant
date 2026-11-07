#pragma once

#include <QHash>
#include <QJsonObject>
#include <QSettings>
#include <QStringList>
#include <QVector>
#include <QWidget>
#include <memory>
#include <optional>

class QLabel;
class QLineEdit;
class QHBoxLayout;
class QPushButton;
class QScrollArea;
class QTimer;

namespace rq {

class BrandMark;
class CandleChart;
class LogoIntro;
class OrderBookView;
class PanelHead;
class PythonEngine;
class StatsStrip;
class TapeView;
class WatchlistView;
struct JobResult;

struct TerminalOptions {
  bool connect = true;              // False keeps it offline, with no network use at all.
  QString startJob = QStringLiteral("terminal_start");  // Tests pass one with synthetic data.
  QString settingsPath;             // Empty uses the normal place. Tests pass a temporary file.
  bool showIntro = true;
};

// The terminal screen, laid out and behaving like the browser terminal.
class TerminalPage : public QWidget {
  Q_OBJECT

 public:
  explicit TerminalPage(PythonEngine& engine, TerminalOptions options = {}, QWidget* parent = nullptr);

  // Runs a command, as if typed into the command box.
  void runCommand(const QString& text);
  void select(const QString& symbol);
  void setTimeframe(const QString& name);

  QString selected() const { return selected_; }
  QString timeframe() const;
  QStringList marketSymbols() const { return markets_; }
  QStringList cryptoSymbols() const { return crypto_; }
  bool isLive() const;
  LogoIntro* intro() const { return intro_; }

 signals:
  void historyRequested(const QString& symbol, bool isMarket);
  void healthRequested();

 protected:
  void resizeEvent(QResizeEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  struct Quote {
    std::optional<double> last, open, high, low, volume, yearHigh, yearLow, cap;
    QString currency, state, name;
    std::optional<double> delay;
    qint64 asOf = 0;
    QVector<double> spark;
    bool dirty = true;
    int flash = 0;
  };
  struct Alert {
    QString symbol;
    double price;
  };
  enum class PendingAction { SelectAfterAdd };

  QWidget* buildTopBar();
  QWidget* buildWatchlist();
  QWidget* buildChartPanel();
  QWidget* buildSidePanel();
  QWidget* buildFooter();
  void createShortcuts();
  void showHelp();

  void onEngineStarted();
  void onEngineFailed(const QString& reason);
  void onJobFinished(const JobResult& result);
  void onEvent(const QJsonObject& message);
  void onLiveEvent(const QJsonObject& message);
  void onMarketsEvent(const QJsonObject& message);

  void applyMode();
  void rebuildTimeframes();
  void loadChart(bool keepView = false);
  void showChart(const JobResult& result);
  void loadDetails();
  void showProfile(const JobResult& result);
  void showNews(const JobResult& result);
  void pushWatch();

  void repaintAll();
  void paintStatus();
  void paintQuote();
  void paintLegend(int index = -1);
  void paintCryptoStats();
  void paintMarketStats();
  void paintWatchRow(const QString& symbol, Quote& quote);

  void addSymbol(const QString& symbol, const QString& kind);
  void addResolved(const QString& kind, const QString& symbol);
  void removeSymbol(const QString& symbol);
  void openSymbol(const QString& word);
  void toast(const QString& message, bool bad = false);
  void checkAlerts(const QString& symbol, std::optional<double> before, double after);
  void showAlert(const QString& text);
  void refreshAlertLines();
  void loadSettings();
  void saveSettings();

  bool isMarket(const QString& symbol) const { return markets_.contains(symbol); }
  Quote& quote(const QString& symbol) { return quotes_[symbol]; }
  QVector<std::pair<QString, int>> timeframes() const;

  PythonEngine& engine_;
  TerminalOptions options_;
  std::unique_ptr<QSettings> settings_;

  QStringList markets_;
  QStringList crypto_;
  QString selected_;
  int granularity_ = 300;
  QVector<Alert> alerts_;
  QHash<QString, Quote> quotes_;

  bool engineRunning_ = false;
  bool connected_ = false;
  bool feedUp_ = false;
  bool everLive_ = false;
  bool hasFeedStatus_ = false;
  bool yahooUp_ = false;
  bool yahooLimited_ = false;
  double retryAtMs_ = 0;
  double lastMessageMs_ = 0;
  QJsonObject session_;
  bool bookReady_ = false;
  int bookDepth_ = 0;
  std::optional<int> fearGreedValue_;
  QString fearGreedNote_ = QStringLiteral("daily index");
  QJsonObject profile_;
  qint64 startedMs_;

  quint64 startJob_ = 0;
  quint64 chartJob_ = 0;
  quint64 profileJob_ = 0;
  quint64 newsJob_ = 0;
  QHash<quint64, QString> resolveJobs_;  // Job id to the words shown if it fails.
  QString chartSymbol_;
  QString chartInterval_;

  BrandMark* brand_;
  QLineEdit* command_;
  QWidget* commandBox_;
  QLabel* cryptoDot_;
  QLabel* cryptoStatus_;
  QLabel* yahooDot_;
  QLabel* yahooStatus_;
  QLabel* meta_;
  QLabel* clock_;
  QPushButton* historyButton_;
  QPushButton* fullButton_;
  QPushButton* helpButton_;
  QWidget* alertBar_;
  QLabel* alertText_;
  WatchlistView* watchlist_;
  QLabel* pairLabel_;
  QLabel* bigPrice_;
  QLabel* bigChange_;
  QLabel* asOf_;
  QHBoxLayout* timeframeLayout_;
  QVector<QPushButton*> timeframeButtons_;
  QLabel* legend_;
  CandleChart* chart_;
  QWidget* bookPane_;
  QWidget* tapePane_;
  QWidget* profilePane_;
  QWidget* newsPane_;
  PanelHead* bookHead_;
  OrderBookView* book_;
  TapeView* tape_;
  QLabel* profileText_;
  PanelHead* newsHead_;
  QLabel* newsText_;
  StatsStrip* stats_;
  QLabel* session_label_;
  LogoIntro* intro_;
  QWidget* help_;
  QTimer* repaintTimer_;
  QTimer* retryTimer_;
  QTimer* refreshTimer_;
  QTimer* toastTimer_;
};

}  // namespace rq
