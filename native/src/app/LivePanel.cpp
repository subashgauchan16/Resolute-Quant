#include "app/LivePanel.h"

#include <QAbstractTableModel>
#include <QBrush>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QSplitter>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>
#include <optional>

#include "app/CandleChart.h"
#include "app/ErrorDisplay.h"
#include "bridge/PythonEngine.h"

namespace rq {

namespace {

constexpr int kStatusRefreshMs = 1000;
constexpr int kChartRetryMs = 15000;
constexpr qint64 kStaleAfterMs = 5000;  // The app is told at least every 2 seconds while streaming.

const QStringList kDefaultSymbols{QStringLiteral("BTC-USD"), QStringLiteral("ETH-USD"), QStringLiteral("SOL-USD"), QStringLiteral("XRP-USD"),
                                  QStringLiteral("DOGE-USD")};
const auto kUp = QStringLiteral("#2bd98a");
const auto kDown = QStringLiteral("#ff5a52");
const auto kWarn = QStringLiteral("#ffd24a");
const auto kMuted = QStringLiteral("#9a9ea6");

std::optional<double> numberIn(const QJsonObject& object, const QString& key) {
  const QJsonValue value = object[key];
  return value.isDouble() ? std::optional<double>(value.toDouble()) : std::nullopt;
}

QString exact(const std::optional<double>& value) {
  return value ? QString::number(*value, 'f', QLocale::FloatingPointShortest) : QString();
}

Candle candleFrom(const QJsonObject& bar) {
  Candle candle;
  candle.time = bar[QStringLiteral("time")].toInteger();
  candle.open = bar[QStringLiteral("open")].toDouble();
  candle.high = bar[QStringLiteral("high")].toDouble();
  candle.low = bar[QStringLiteral("low")].toDouble();
  candle.close = bar[QStringLiteral("close")].toDouble();
  candle.hasVolume = bar[QStringLiteral("volume")].isDouble();
  candle.volume = bar[QStringLiteral("volume")].toDouble();
  return candle;
}

}  // namespace

// One row per pair. Prices turn grey when they are not live, so old numbers can't pass as current.
class WatchlistModel : public QAbstractTableModel {
 public:
  struct Row {
    QString symbol;
    std::optional<double> price, open, bid, ask;
  };

  using QAbstractTableModel::QAbstractTableModel;

  const QVector<Row>& rows() const { return rows_; }

  void setSymbols(const QStringList& symbols) {
    beginResetModel();
    QVector<Row> next;
    for (const QString& symbol : symbols) {
      const auto found = std::find_if(rows_.cbegin(), rows_.cend(), [&](const Row& row) { return row.symbol == symbol; });
      next.push_back(found != rows_.cend() ? *found : Row{symbol, {}, {}, {}, {}});
    }
    rows_ = std::move(next);
    endResetModel();
  }

  void updateTicker(const QJsonObject& ticker) {
    const QString symbol = ticker[QStringLiteral("id")].toString();
    for (int row = 0; row < rows_.size(); ++row) {
      if (rows_[row].symbol != symbol) continue;
      Row& target = rows_[row];
      target.price = numberIn(ticker, QStringLiteral("price"));
      if (const auto open = numberIn(ticker, QStringLiteral("open"))) target.open = open;
      if (const auto bid = numberIn(ticker, QStringLiteral("bid"))) target.bid = bid;
      if (const auto ask = numberIn(ticker, QStringLiteral("ask"))) target.ask = ask;
      emit dataChanged(index(row, 0), index(row, columnCount() - 1));
      return;
    }
  }

  void setLive(bool live) {
    if (live == live_) return;
    live_ = live;
    if (!rows_.isEmpty()) emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1), {Qt::ForegroundRole});
  }

  int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : static_cast<int>(rows_.size()); }
  int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : 5; }

  QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal) return {};
    static const char* const names[] = {"Pair", "Last", "24h", "Bid", "Ask"};
    return QString::fromLatin1(names[section]);
  }

  QVariant data(const QModelIndex& index, int role) const override {
    if (!index.isValid()) return {};
    const Row& row = rows_[index.row()];
    const std::optional<double> change = row.price && row.open && *row.open != 0.0
                                              ? std::optional<double>((*row.price - *row.open) / *row.open * 100.0)
                                              : std::nullopt;
    if (role == Qt::TextAlignmentRole) {
      return index.column() == 0 ? QVariant(Qt::AlignLeft | Qt::AlignVCenter) : QVariant(Qt::AlignRight | Qt::AlignVCenter);
    }
    if (role == Qt::ForegroundRole && index.column() > 0) {
      if (!live_) return QBrush(QColor(kMuted));
      if (index.column() == 2 && change) return QBrush(QColor(*change >= 0.0 ? kUp : kDown));
      return {};
    }
    if (role != Qt::DisplayRole) return {};
    switch (index.column()) {
      case 0:
        return row.symbol;
      case 1:
        return exact(row.price);
      case 2:
        return change ? QStringLiteral("%1%2%").arg(*change >= 0.0 ? QStringLiteral("+") : QString()).arg(*change, 0, 'f', 2) : QString();
      case 3:
        return exact(row.bid);
      default:
        return exact(row.ask);
    }
  }

 private:
  QVector<Row> rows_;
  bool live_ = false;
};

LivePanel::LivePanel(PythonEngine& engine, QString startJob, QWidget* parent)
    : QWidget(parent),
      engine_(engine),
      startJob_(std::move(startJob)),
      startButton_(new QPushButton(QStringLiteral("Go live"))),
      stopButton_(new QPushButton(QStringLiteral("Stop"))),
      status_(new QLabel(QStringLiteral("Waiting for Python to start."))),
      model_(new WatchlistModel(this)),
      table_(new QTableView),
      addField_(new QLineEdit),
      addButton_(new QPushButton(QStringLiteral("Add"))),
      removeButton_(new QPushButton(QStringLiteral("Remove"))),
      chartTitle_(new QLabel),
      interval_(new QComboBox),
      chart_(new CandleChart),
      error_(new ErrorDisplay(QStringLiteral("liveError"))),
      statusTimer_(new QTimer(this)),
      retryTimer_(new QTimer(this)) {
  startButton_->setObjectName(QStringLiteral("liveStart"));
  stopButton_->setObjectName(QStringLiteral("liveStop"));
  status_->setObjectName(QStringLiteral("liveStatus"));
  status_->setWordWrap(true);
  table_->setObjectName(QStringLiteral("liveTable"));
  addField_->setObjectName(QStringLiteral("liveAddField"));
  addField_->setPlaceholderText(QStringLiteral("Add a pair, such as LTC-USD"));
  addButton_->setObjectName(QStringLiteral("liveAdd"));
  removeButton_->setObjectName(QStringLiteral("liveRemove"));
  chartTitle_->setObjectName(QStringLiteral("liveChartTitle"));
  interval_->setObjectName(QStringLiteral("liveInterval"));
  chart_->setObjectName(QStringLiteral("liveChart"));
  chart_->clear(QStringLiteral("Press Go live to stream prices from Coinbase."));
  startButton_->setEnabled(false);
  stopButton_->setEnabled(false);
  addButton_->setEnabled(false);

  table_->setModel(model_);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);
  table_->verticalHeader()->setVisible(false);
  table_->verticalHeader()->setDefaultSectionSize(26);
  table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
  table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  table_->setMinimumWidth(340);
  table_->setShowGrid(false);
  model_->setSymbols(kDefaultSymbols);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(24, 20, 24, 12);

  auto* header = new QHBoxLayout;
  auto* title = new QLabel(QStringLiteral("Live prices"));
  title->setObjectName(QStringLiteral("pageTitle"));
  header->addWidget(title);
  header->addSpacing(16);
  header->addWidget(status_, 1);
  header->addWidget(startButton_);
  header->addWidget(stopButton_);
  layout->addLayout(header);
  layout->addWidget(error_);

  auto* left = new QWidget;
  auto* leftLayout = new QVBoxLayout(left);
  leftLayout->setContentsMargins(0, 0, 0, 0);
  leftLayout->addWidget(table_, 1);
  auto* addRow = new QHBoxLayout;
  addRow->addWidget(addField_, 1);
  addRow->addWidget(addButton_);
  addRow->addWidget(removeButton_);
  leftLayout->addLayout(addRow);

  auto* right = new QWidget;
  auto* rightLayout = new QVBoxLayout(right);
  rightLayout->setContentsMargins(0, 0, 0, 0);
  auto* chartHeader = new QHBoxLayout;
  chartHeader->addWidget(chartTitle_, 1);
  chartHeader->addWidget(interval_);
  rightLayout->addLayout(chartHeader);
  rightLayout->addWidget(chart_, 1);

  auto* splitter = new QSplitter(Qt::Horizontal);
  splitter->addWidget(left);
  splitter->addWidget(right);
  splitter->setStretchFactor(0, 2);
  splitter->setStretchFactor(1, 5);
  splitter->setChildrenCollapsible(false);
  splitter->setSizes({400, 900});
  layout->addWidget(splitter, 1);

  auto* note = new QLabel(QStringLiteral("Coinbase prices only, for personal research. The newest bar is still forming, and its volume counts only "
                                         "trades seen since the chart loaded. Yahoo symbols are not streamed here yet."));
  note->setObjectName(QStringLiteral("muted"));
  note->setWordWrap(true);
  layout->addWidget(note);

  connect(startButton_, &QPushButton::clicked, this, &LivePanel::goLive);
  connect(stopButton_, &QPushButton::clicked, this, &LivePanel::stopLive);
  connect(addButton_, &QPushButton::clicked, this, &LivePanel::addSymbol);
  connect(addField_, &QLineEdit::returnPressed, this, &LivePanel::addSymbol);
  connect(removeButton_, &QPushButton::clicked, this, &LivePanel::removeSelected);
  connect(table_->selectionModel(), &QItemSelectionModel::currentRowChanged, this, &LivePanel::onSelectionChanged);
  connect(interval_, &QComboBox::currentIndexChanged, this, &LivePanel::loadChart);
  connect(&engine_, &PythonEngine::started, this, &LivePanel::onStarted);
  connect(&engine_, &PythonEngine::startFailed, this, &LivePanel::onStartFailed);
  connect(&engine_, &PythonEngine::jobFinished, this, &LivePanel::onJobFinished);
  connect(&engine_, &PythonEngine::pythonEvent, this, &LivePanel::onEvent);
  connect(statusTimer_, &QTimer::timeout, this, &LivePanel::refreshStatus);
  statusTimer_->start(kStatusRefreshMs);
  retryTimer_->setSingleShot(true);
  connect(retryTimer_, &QTimer::timeout, this, &LivePanel::loadChart);

  table_->selectRow(0);
}

QString LivePanel::selectedSymbol() const {
  const int row = table_->currentIndex().row();
  return row >= 0 && row < model_->rows().size() ? model_->rows()[row].symbol : QString();
}

QStringList LivePanel::symbols() const {
  QStringList list;
  for (const auto& row : model_->rows()) list << row.symbol;
  return list;
}

bool LivePanel::isLive() const {
  if (!streaming_ || !feedStatus_[QStringLiteral("connected")].toBool() || !lastEventAt_.isValid()) return false;
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  const qint64 lastMessage = feedStatus_[QStringLiteral("lastMessage")].toInteger();
  return lastEventAt_.msecsTo(QDateTime::currentDateTimeUtc()) < kStaleAfterMs && now - lastMessage < kStaleAfterMs;
}

void LivePanel::setStatus(const QString& text, const QString& color) {
  status_->setText(text);
  status_->setStyleSheet(QStringLiteral("color: %1;").arg(color));
}

void LivePanel::refreshStatus() {
  const bool live = isLive();
  model_->setLive(live);
  if (!pythonRunning_) return;
  if (!streaming_) {
    setStatus(QStringLiteral("Not connected. Prices shown, if any, are not updating."), kMuted);
  } else if (live) {
    const int reconnects = feedStatus_[QStringLiteral("reconnects")].toInt();
    setStatus(reconnects > 0 ? QStringLiteral("Live. Reconnected %1 times.").arg(reconnects) : QStringLiteral("Live"), kUp);
  } else if (feedStatus_.isEmpty()) {
    setStatus(QStringLiteral("Connecting to Coinbase."), kWarn);
  } else if (!feedStatus_[QStringLiteral("connected")].toBool()) {
    setStatus(QStringLiteral("Disconnected. Retrying automatically. Prices are not updating."), kWarn);
  } else {
    setStatus(QStringLiteral("No new data from Coinbase. Prices are not updating."), kWarn);
  }
}

void LivePanel::onStarted() {
  pythonRunning_ = true;
  optionsJob_ = engine_.submit(QStringLiteral("history_options"));
}

void LivePanel::onStartFailed(const QString& reason) {
  setStatus(QStringLiteral("Live prices need Python, which is unavailable."), kWarn);
  error_->showError(reason);
}

void LivePanel::goLive() {
  error_->clearError();
  startButton_->setEnabled(false);
  stopButton_->setEnabled(true);
  feedStatus_ = {};
  lastEventAt_ = {};
  startingJob_ = engine_.submit(startJob_, QJsonObject{{QStringLiteral("symbols"), QJsonArray::fromStringList(symbols())}});
  streaming_ = true;
  refreshStatus();
  loadChart();
}

void LivePanel::stopLive() {
  stopButton_->setEnabled(false);
  retryTimer_->stop();
  stoppingJob_ = engine_.submit(QStringLiteral("live_stop"));
  streaming_ = false;
  refreshStatus();
}

void LivePanel::addSymbol() {
  const QString symbol = addField_->text().trimmed().toUpper();
  if (symbol.isEmpty() || addJob_ != 0) return;
  error_->clearError();
  addButton_->setEnabled(false);
  addJob_ = engine_.submit(QStringLiteral("live_add"), QJsonObject{{QStringLiteral("symbols"), QJsonArray::fromStringList(symbols())},
                                                                   {QStringLiteral("symbol"), symbol}});
}

void LivePanel::removeSelected() {
  const QString symbol = selectedSymbol();
  if (symbol.isEmpty() || model_->rowCount() <= 1) return;
  QStringList remaining = symbols();
  remaining.removeAll(symbol);
  const int row = std::min<int>(table_->currentIndex().row(), static_cast<int>(remaining.size()) - 1);
  model_->setSymbols(remaining);
  table_->selectRow(row);
  watchJob_ = engine_.submit(QStringLiteral("live_watch"), QJsonObject{{QStringLiteral("symbols"), QJsonArray::fromStringList(remaining)}});
}

void LivePanel::onSelectionChanged() { loadChart(); }

void LivePanel::loadChart() {
  const QString symbol = selectedSymbol();
  const QString interval = interval_->currentText();
  chartTitle_->setText(symbol);
  if (!streaming_ || symbol.isEmpty() || interval.isEmpty()) return;
  retryTimer_->stop();
  // A pair's candles must never stay on screen under another pair's name.
  if (symbol != chartSymbol_ || interval != chartInterval_) {
    chartSymbol_ = symbol;
    chartInterval_ = interval;
    chart_->clear(QStringLiteral("Loading %1.").arg(symbol));
  }
  chartJob_ = engine_.submit(QStringLiteral("live_chart"), QJsonObject{{QStringLiteral("symbol"), symbol}, {QStringLiteral("interval"), interval}});
}

void LivePanel::onJobFinished(const JobResult& result) {
  if (result.id == optionsJob_) {
    optionsJob_ = 0;
    if (result.status != JobStatus::Ok) {
      showFailure(result);
      return;
    }
    const QSignalBlocker blocker(interval_);
    for (const auto& entry : result.value[QStringLiteral("sources")].toArray()) {
      const QJsonObject source = entry.toObject();
      if (source[QStringLiteral("id")].toString() != QStringLiteral("coinbase")) continue;
      for (const auto& interval : source[QStringLiteral("intervals")].toArray()) interval_->addItem(interval.toString());
    }
    interval_->setCurrentIndex(std::max(interval_->findText(QStringLiteral("1M")), 0));
    startButton_->setEnabled(true);
    addButton_->setEnabled(true);
    refreshStatus();
  } else if (result.id == startingJob_) {
    startingJob_ = 0;
    if (result.status != JobStatus::Ok) {
      streaming_ = false;
      startButton_->setEnabled(true);
      stopButton_->setEnabled(false);
      showFailure(result);
      refreshStatus();
    }
  } else if (result.id == stoppingJob_) {
    stoppingJob_ = 0;
    startButton_->setEnabled(pythonRunning_);
    if (result.status != JobStatus::Ok) showFailure(result);
  } else if (result.id == chartJob_) {
    chartJob_ = 0;
    showChart(result);
  } else if (result.id == addJob_) {
    addJob_ = 0;
    addButton_->setEnabled(true);
    if (result.status != JobStatus::Ok) {
      showFailure(result);
      return;
    }
    QStringList list;
    for (const auto& symbol : result.value[QStringLiteral("symbols")].toArray()) list << symbol.toString();
    const QString selected = selectedSymbol();
    model_->setSymbols(list);
    table_->selectRow(std::max<int>(static_cast<int>(list.indexOf(selected)), 0));
    addField_->clear();
  } else if (result.id == watchJob_) {
    watchJob_ = 0;
    if (result.status != JobStatus::Ok) showFailure(result);
  }
}

void LivePanel::showChart(const JobResult& result) {
  if (result.status != JobStatus::Ok) {
    if (result.status == JobStatus::Error) {
      chart_->clear(QStringLiteral("Bars for %1 did not load. Retrying in %2 seconds.").arg(chartSymbol_).arg(kChartRetryMs / 1000));
      showFailure(result);
      if (streaming_) retryTimer_->start(kChartRetryMs);
    }
    return;
  }
  error_->clearError();
  const QString symbol = result.value[QStringLiteral("symbol")].toString();
  const QString interval = result.value[QStringLiteral("interval")].toString();
  if (symbol != chartSymbol_ || interval != chartInterval_) return;  // The person has moved on.
  QVector<Candle> candles;
  const QJsonArray bars = result.value[QStringLiteral("bars")].toArray();
  candles.reserve(bars.size());
  for (const auto& bar : bars) candles.push_back(candleFrom(bar.toObject()));
  chart_->setCandles(std::move(candles), interval != QStringLiteral("1D"));
}

void LivePanel::onEvent(const QJsonObject& message) {
  if (message[QStringLiteral("type")].toString() != QStringLiteral("live")) return;
  if (message[QStringLiteral("stopped")].toBool()) {
    feedStatus_ = {};
    lastEventAt_ = {};
    refreshStatus();
    return;
  }
  if (!streaming_) return;
  lastEventAt_ = QDateTime::currentDateTimeUtc();
  if (message.contains(QStringLiteral("status"))) feedStatus_ = message[QStringLiteral("status")].toObject();
  for (const auto& ticker : message[QStringLiteral("tickers")].toArray()) model_->updateTicker(ticker.toObject());
  if (message.contains(QStringLiteral("bar"))) {
    const QJsonObject bar = message[QStringLiteral("bar")].toObject();
    if (bar[QStringLiteral("symbol")].toString() == chartSymbol_ && bar[QStringLiteral("interval")].toString() == chartInterval_) {
      chart_->updateLatest(candleFrom(bar));
    }
  }
  if (message[QStringLiteral("restored")].toBool()) loadChart();  // Trades were missed while disconnected.
  refreshStatus();
}

void LivePanel::showFailure(const JobResult& result) {
  const bool plain = result.errorType == QStringLiteral("BadRequest") || result.errorType == QStringLiteral("ProviderError");
  error_->showError(plain ? result.message : QStringLiteral("%1: %2").arg(result.errorType, result.message), plain ? QString() : result.traceback);
}

}  // namespace rq
