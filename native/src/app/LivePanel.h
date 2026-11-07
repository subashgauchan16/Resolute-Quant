#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QStringList>
#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableView;
class QTimer;

namespace rq {

class CandleChart;
class ErrorDisplay;
class PythonEngine;
class WatchlistModel;
struct JobResult;

// A live Coinbase watchlist, with a chart of the selected pair whose newest bar updates as trades arrive.
// Nothing connects until the person presses Go live.
class LivePanel : public QWidget {
  Q_OBJECT

 public:
  // Tests pass a different start job, which streams synthetic prices with no network.
  explicit LivePanel(PythonEngine& engine, QString startJob = QStringLiteral("live_start"), QWidget* parent = nullptr);

  // True while connected and receiving messages. Used to label prices as live or not.
  bool isLive() const;

 private:
  void onStarted();
  void onStartFailed(const QString& reason);
  void onJobFinished(const JobResult& result);
  void onEvent(const QJsonObject& message);
  void goLive();
  void stopLive();
  void addSymbol();
  void removeSelected();
  void onSelectionChanged();
  void loadChart();
  void showChart(const JobResult& result);
  void showFailure(const JobResult& result);
  void refreshStatus();
  void setStatus(const QString& text, const QString& color);
  QString selectedSymbol() const;
  QStringList symbols() const;

  PythonEngine& engine_;
  QString startJob_;
  quint64 optionsJob_ = 0;
  quint64 startingJob_ = 0;
  quint64 stoppingJob_ = 0;
  quint64 chartJob_ = 0;
  quint64 addJob_ = 0;
  quint64 watchJob_ = 0;
  bool pythonRunning_ = false;
  bool streaming_ = false;

  QString chartSymbol_;    // What the chart currently shows, so stray bars can't land on it.
  QString chartInterval_;

  QJsonObject feedStatus_;
  QDateTime lastEventAt_;

  QPushButton* startButton_;
  QPushButton* stopButton_;
  QLabel* status_;
  WatchlistModel* model_;
  QTableView* table_;
  QLineEdit* addField_;
  QPushButton* addButton_;
  QPushButton* removeButton_;
  QLabel* chartTitle_;
  QComboBox* interval_;
  CandleChart* chart_;
  ErrorDisplay* error_;
  QTimer* statusTimer_;
  QTimer* retryTimer_;
};

}  // namespace rq
