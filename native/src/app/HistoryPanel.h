#pragma once

#include <QJsonArray>
#include <QWidget>

class QComboBox;
class QDateEdit;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableView;

namespace rq {

class CandleChart;
class ErrorDisplay;
class HistoryTableModel;
class PythonEngine;
struct JobResult;

// Choose a symbol, timeframe and dates, then read the bars as a chart and a table, and export them.
class HistoryPanel : public QWidget {
  Q_OBJECT

 public:
  // Tests pass a different load job, so they never contact a provider.
  explicit HistoryPanel(PythonEngine& engine, QString loadJob = QStringLiteral("history"), QWidget* parent = nullptr);

  // Writes the bars on screen to path. The Export CSV button asks for the path, then calls this.
  void exportTo(const QString& path);
  // Opens on the terminal's current symbol. It loads when the person presses Load history.
  void openFor(const QString& symbol, bool isMarket);

 signals:
  void closeRequested();

 private:
  void onStarted();
  void onStartFailed(const QString& reason);
  void onJobFinished(const JobResult& result);
  void onSourceChanged();
  void load();
  void cancelLoad();
  void chooseExportPath();
  void showOptions(const JobResult& result);
  void showHistory(const JobResult& result);
  void showExport(const JobResult& result);
  void showFailure(const JobResult& result);
  void setBusy(bool busy);

  PythonEngine& engine_;
  QString loadJob_;
  QJsonArray sources_;
  QString previousExample_;
  QString exportName_;
  quint64 optionsJob_ = 0;
  quint64 historyJob_ = 0;
  quint64 exportJob_ = 0;
  bool haveOptions_ = false;
  bool haveView_ = false;

  QComboBox* source_;
  QLineEdit* symbol_;
  QComboBox* interval_;
  QDateEdit* start_;
  QDateEdit* end_;
  QPushButton* loadButton_;
  QPushButton* cancelButton_;
  QPushButton* exportButton_;
  QLabel* status_;
  QLabel* warnings_;
  ErrorDisplay* error_;
  CandleChart* chart_;
  HistoryTableModel* model_;
  QTableView* table_;
};

}  // namespace rq
