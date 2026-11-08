#include "app/HistoryPanel.h"

#include <QAbstractTableModel>
#include <QComboBox>
#include <QDateEdit>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QSplitter>
#include <QStandardPaths>
#include <QTableView>
#include <QTimeZone>
#include <QVBoxLayout>
#include <algorithm>

#include "app/CandleChart.h"
#include "app/ErrorDisplay.h"
#include "bridge/PythonEngine.h"

namespace rq {

// The bars as a table, in the order given: newest first.
class HistoryTableModel : public QAbstractTableModel {
 public:
  using QAbstractTableModel::QAbstractTableModel;

  void setCandles(QVector<Candle> candles, bool intraday) {
    beginResetModel();
    candles_ = std::move(candles);
    intraday_ = intraday;
    endResetModel();
  }

  int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : static_cast<int>(candles_.size()); }
  int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : 6; }

  QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
    if (role != Qt::DisplayRole) return {};
    if (orientation == Qt::Vertical) return section + 1;
    static const char* const names[] = {"Time (UTC)", "Open", "High", "Low", "Close", "Volume"};
    return QString::fromLatin1(names[section]);
  }

  QVariant data(const QModelIndex& index, int role) const override {
    if (!index.isValid()) return {};
    if (role == Qt::TextAlignmentRole) {
      return index.column() == 0 ? QVariant(Qt::AlignLeft | Qt::AlignVCenter) : QVariant(Qt::AlignRight | Qt::AlignVCenter);
    }
    if (role != Qt::DisplayRole) return {};
    const Candle& c = candles_[index.row()];
    const auto number = [](double value) { return QString::number(value, 'f', QLocale::FloatingPointShortest); };
    switch (index.column()) {
      case 0:
        return QDateTime::fromSecsSinceEpoch(c.time, QTimeZone::UTC)
            .toString(intraday_ ? QStringLiteral("yyyy-MM-dd HH:mm") : QStringLiteral("yyyy-MM-dd"));
      case 1:
        return number(c.open);
      case 2:
        return number(c.high);
      case 3:
        return number(c.low);
      case 4:
        return number(c.close);
      default:
        return c.hasVolume ? number(c.volume) : QString();  // Missing stays blank, never zero.
    }
  }

 private:
  QVector<Candle> candles_;
  bool intraday_ = false;
};

namespace {

const auto kPrompt = QStringLiteral("Choose a symbol and dates, then load the history.");

QDateEdit* makeDate(const QString& name, const QDate& date) {
  auto* edit = new QDateEdit(date);
  edit->setObjectName(name);
  edit->setCalendarPopup(true);
  edit->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
  edit->setMaximumDate(QDateTime::currentDateTimeUtc().date());
  return edit;
}

QWidget* labelled(const QString& text, QWidget* field) {
  auto* box = new QWidget;
  auto* layout = new QVBoxLayout(box);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(3);
  auto* label = new QLabel(text);
  label->setObjectName(QStringLiteral("muted"));
  label->setBuddy(field);
  layout->addWidget(label);
  layout->addWidget(field);
  return box;
}

QString whole(int count) { return QLocale::c().toString(count); }

QString countOf(int count, const char* noun) {
  return QStringLiteral("%1 %2%3").arg(whole(count), QString::fromLatin1(noun), count == 1 ? QString() : QStringLiteral("s"));
}

}  // namespace

HistoryPanel::HistoryPanel(PythonEngine& engine, QString loadJob, QWidget* parent)
    : QWidget(parent),
      engine_(engine),
      loadJob_(std::move(loadJob)),
      source_(new QComboBox),
      symbol_(new QLineEdit),
      interval_(new QComboBox),
      start_(makeDate(QStringLiteral("historyStart"), QDateTime::currentDateTimeUtc().date().addDays(-30))),
      end_(makeDate(QStringLiteral("historyEnd"), QDateTime::currentDateTimeUtc().date())),
      loadButton_(new QPushButton(QStringLiteral("Load history"))),
      cancelButton_(new QPushButton(QStringLiteral("Cancel"))),
      exportButton_(new QPushButton(QStringLiteral("Export CSV"))),
      status_(new QLabel(QStringLiteral("Waiting for Python to start."))),
      warnings_(new QLabel),
      error_(new ErrorDisplay(QStringLiteral("historyError"))),
      chart_(new CandleChart),
      model_(new HistoryTableModel(this)),
      table_(new QTableView) {
  source_->setObjectName(QStringLiteral("historySource"));
  symbol_->setObjectName(QStringLiteral("historySymbol"));
  symbol_->setMinimumWidth(110);
  interval_->setObjectName(QStringLiteral("historyInterval"));
  loadButton_->setObjectName(QStringLiteral("historyLoad"));
  cancelButton_->setObjectName(QStringLiteral("historyCancel"));
  exportButton_->setObjectName(QStringLiteral("historyExport"));
  status_->setObjectName(QStringLiteral("historyStatus"));
  status_->setWordWrap(true);
  status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  warnings_->setObjectName(QStringLiteral("historyWarnings"));
  warnings_->setStyleSheet(QStringLiteral("color: #ffd24a;"));
  warnings_->setWordWrap(true);
  warnings_->hide();
  chart_->setObjectName(QStringLiteral("historyChart"));
  chart_->clear(kPrompt);

  table_->setObjectName(QStringLiteral("historyTable"));
  table_->setModel(model_);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setAlternatingRowColors(false);
  table_->verticalHeader()->setVisible(false);
  table_->verticalHeader()->setDefaultSectionSize(24);
  table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(24, 20, 24, 12);

  auto* title = new QLabel(QStringLiteral("HISTORY"));
  title->setObjectName(QStringLiteral("pageTitle"));
  auto* titleRow = new QHBoxLayout;
  auto* closeButton = new QPushButton(QStringLiteral("Close"));
  closeButton->setObjectName(QStringLiteral("historyClose"));
  connect(closeButton, &QPushButton::clicked, this, &HistoryPanel::closeRequested);
  titleRow->addWidget(title);
  titleRow->addStretch();
  titleRow->addWidget(closeButton);
  layout->addLayout(titleRow);

  auto* controls = new QHBoxLayout;
  controls->addWidget(labelled(QStringLiteral("Source"), source_));
  controls->addWidget(labelled(QStringLiteral("Symbol"), symbol_));
  controls->addWidget(labelled(QStringLiteral("Timeframe"), interval_));
  controls->addWidget(labelled(QStringLiteral("From (UTC)"), start_));
  controls->addWidget(labelled(QStringLiteral("To (UTC)"), end_));
  controls->addStretch();
  controls->addWidget(loadButton_, 0, Qt::AlignBottom);
  controls->addWidget(cancelButton_, 0, Qt::AlignBottom);
  controls->addWidget(exportButton_, 0, Qt::AlignBottom);
  layout->addLayout(controls);
  layout->addWidget(status_);
  layout->addWidget(warnings_);
  layout->addWidget(error_);

  auto* splitter = new QSplitter(Qt::Vertical);
  splitter->addWidget(chart_);
  splitter->addWidget(table_);
  splitter->setStretchFactor(0, 3);
  splitter->setStretchFactor(1, 2);
  splitter->setChildrenCollapsible(false);
  layout->addWidget(splitter, 1);

  auto* note = new QLabel(QStringLiteral("Prices come from the provider as they are. Yahoo prices are not adjusted for "
                                         "splits or dividends. Provider data is for personal research."));
  note->setObjectName(QStringLiteral("muted"));
  note->setWordWrap(true);
  layout->addWidget(note);

  setBusy(false);
  connect(loadButton_, &QPushButton::clicked, this, &HistoryPanel::load);
  connect(symbol_, &QLineEdit::returnPressed, this, &HistoryPanel::load);
  connect(cancelButton_, &QPushButton::clicked, this, &HistoryPanel::cancelLoad);
  connect(exportButton_, &QPushButton::clicked, this, &HistoryPanel::chooseExportPath);
  connect(source_, &QComboBox::currentIndexChanged, this, &HistoryPanel::onSourceChanged);
  connect(&engine_, &PythonEngine::started, this, &HistoryPanel::onStarted);
  connect(&engine_, &PythonEngine::startFailed, this, &HistoryPanel::onStartFailed);
  connect(&engine_, &PythonEngine::jobFinished, this, &HistoryPanel::onJobFinished);
}

void HistoryPanel::openFor(const QString& symbol, bool isMarket) {
  if (!haveOptions_) return;
  source_->setCurrentIndex(std::max(source_->findData(isMarket ? QStringLiteral("yahoo") : QStringLiteral("coinbase")), 0));
  symbol_->setText(symbol);
}

void HistoryPanel::setBusy(bool busy) {
  loadButton_->setEnabled(haveOptions_ && !busy);
  cancelButton_->setEnabled(historyJob_ != 0);
  exportButton_->setEnabled(haveView_ && !busy);
}

void HistoryPanel::onStarted() { optionsJob_ = engine_.submit(QStringLiteral("history_options")); }

void HistoryPanel::onStartFailed(const QString& reason) {
  status_->setText(QStringLiteral("History needs Python, which is unavailable."));
  error_->showError(reason);
}

void HistoryPanel::onJobFinished(const JobResult& result) {
  if (result.id == optionsJob_) {
    optionsJob_ = 0;
    showOptions(result);
  } else if (result.id == historyJob_) {
    historyJob_ = 0;
    showHistory(result);
  } else if (result.id == exportJob_) {
    exportJob_ = 0;
    showExport(result);
  }
}

void HistoryPanel::showOptions(const JobResult& result) {
  if (result.status != JobStatus::Ok) {
    status_->setText(QStringLiteral("History could not start."));
    showFailure(result);
    return;
  }
  sources_ = result.value[QStringLiteral("sources")].toArray();
  const QSignalBlocker blocker(source_);
  for (const auto& entry : sources_) {
    const QJsonObject source = entry.toObject();
    source_->addItem(source[QStringLiteral("label")].toString(), source[QStringLiteral("id")].toString());
  }
  haveOptions_ = true;
  onSourceChanged();
  status_->setText(kPrompt);
  setBusy(false);
}

void HistoryPanel::onSourceChanged() {
  const QJsonObject source = sources_.at(source_->currentIndex()).toObject();
  const QString kept = interval_->currentText();
  interval_->clear();
  for (const auto& interval : source[QStringLiteral("intervals")].toArray()) interval_->addItem(interval.toString());
  const int match = interval_->findText(kept.isEmpty() ? QStringLiteral("1D") : kept);
  interval_->setCurrentIndex(match >= 0 ? match : interval_->findText(QStringLiteral("1D")));

  // Replace the example symbol, but never something the person typed.
  const QString example = source[QStringLiteral("example")].toString();
  if (symbol_->text().isEmpty() || symbol_->text() == previousExample_) symbol_->setText(example);
  previousExample_ = example;
}

void HistoryPanel::load() {
  if (!haveOptions_ || historyJob_ != 0 || exportJob_ != 0) return;
  // Clear first, so the last symbol's bars can never sit under a new symbol's name.
  haveView_ = false;
  exportName_.clear();
  chart_->clear(QStringLiteral("Loading."));
  model_->setCandles({}, false);
  error_->clearError();
  warnings_->hide();
  status_->setText(QStringLiteral("Loading %1.").arg(symbol_->text().trimmed().toUpper()));

  const auto day = [](const QDateEdit* edit) { return edit->date().toString(Qt::ISODate); };
  historyJob_ = engine_.submit(loadJob_, QJsonObject{{QStringLiteral("source"), source_->currentData().toString()},
                                                     {QStringLiteral("symbol"), symbol_->text()},
                                                     {QStringLiteral("interval"), interval_->currentText()},
                                                     {QStringLiteral("start"), day(start_)},
                                                     {QStringLiteral("end"), day(end_)}});
  setBusy(true);
}

void HistoryPanel::cancelLoad() {
  if (historyJob_ == 0) return;
  cancelButton_->setEnabled(false);
  status_->setText(QStringLiteral("Cancelling. A request already sent to the provider must finish first."));
  engine_.cancel(historyJob_);
}

void HistoryPanel::showHistory(const JobResult& result) {
  if (result.status == JobStatus::Cancelled) {
    chart_->clear(QStringLiteral("Cancelled. Nothing was loaded."));
    status_->setText(QStringLiteral("Cancelled."));
    setBusy(false);
    return;
  }
  if (result.status != JobStatus::Ok) {
    chart_->clear(QStringLiteral("Nothing to show. The request failed."));
    status_->setText(QStringLiteral("The history did not load."));
    showFailure(result);
    setBusy(false);
    return;
  }

  const QJsonObject& view = result.value;
  QVector<Candle> newestFirst;
  const QJsonArray bars = view[QStringLiteral("bars")].toArray();
  newestFirst.reserve(bars.size());
  for (const auto& entry : bars) {
    const QJsonObject bar = entry.toObject();
    Candle candle;
    candle.time = bar[QStringLiteral("time")].toInteger();
    candle.open = bar[QStringLiteral("open")].toDouble();
    candle.high = bar[QStringLiteral("high")].toDouble();
    candle.low = bar[QStringLiteral("low")].toDouble();
    candle.close = bar[QStringLiteral("close")].toDouble();
    const QJsonValue volume = bar[QStringLiteral("volume")];
    candle.hasVolume = volume.isDouble();
    candle.volume = volume.toDouble();
    newestFirst.push_back(candle);
  }

  const QString interval = view[QStringLiteral("interval")].toString();
  const bool intraday = interval != QStringLiteral("1D") && interval != QStringLiteral("1W") && interval != QStringLiteral("1MO");
  QVector<Candle> oldestFirst(newestFirst.crbegin(), newestFirst.crend());
  chart_->setCandles(std::move(oldestFirst), intraday);
  model_->setCandles(std::move(newestFirst), intraday);

  const int count = view[QStringLiteral("count")].toInt();
  const QString symbol = view[QStringLiteral("symbol")].toString();
  const QString source = source_->findData(view[QStringLiteral("source")].toString()) >= 0
                             ? source_->itemText(source_->findData(view[QStringLiteral("source")].toString()))
                             : view[QStringLiteral("source")].toString();
  if (count == 0) {
    chart_->clear(QStringLiteral("The provider returned no finished bars for that request."));
    status_->setText(QStringLiteral("%1 %2: no finished bars. Source: %3. Check the symbol and dates.").arg(symbol, interval, source));
  } else {
    status_->setText(QStringLiteral("%1 %2: %3, %4 to %5. Source: %6. Downloaded %7.")
                         .arg(symbol, interval, countOf(count, "bar"), view[QStringLiteral("first_utc")].toString(),
                              view[QStringLiteral("last_utc")].toString(), source,
                              view[QStringLiteral("downloaded_at_utc")].toString()));
    haveView_ = true;
    exportName_ = view[QStringLiteral("export_name")].toString();
  }

  QStringList notes;
  if (view[QStringLiteral("truncated")].toBool()) {
    notes << QStringLiteral("Only the newest %1 bars are shown. Choose a shorter range or a longer timeframe to see the rest.")
                 .arg(whole(view[QStringLiteral("limit")].toInt()));
  }
  const QJsonObject gaps = view[QStringLiteral("gaps")].toObject();
  const int missing = gaps[QStringLiteral("missing_bars")].toInt();
  if (missing > 0) {
    const bool sessions = gaps[QStringLiteral("scope")].toString() == QStringLiteral("within_trading_days");
    notes << QStringLiteral("Missing: %1 across %2%3. Gaps are left empty, never filled.")
                 .arg(countOf(missing, "bar"), countOf(gaps[QStringLiteral("count")].toInt(), "gap"),
                      sessions ? QStringLiteral(" within trading days") : QString());
  }
  const int rejected = view[QStringLiteral("rejected")].toInt();
  if (rejected > 0) notes << QStringLiteral("Dropped after failing validation: %1.").arg(countOf(rejected, "bar"));
  const int unfinished = view[QStringLiteral("unfinished_excluded")].toInt();
  if (unfinished > 0) notes << QStringLiteral("Left out because still changing: %1.").arg(countOf(unfinished, "unfinished bar"));
  warnings_->setText(notes.join(QLatin1Char(' ')));
  warnings_->setVisible(!notes.isEmpty());
  setBusy(false);
}

void HistoryPanel::chooseExportPath() {
  const QString folder = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
  const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export CSV"), folder + QLatin1Char('/') + exportName_,
                                                    QStringLiteral("CSV files (*.csv)"));
  if (!path.isEmpty()) exportTo(path);
}

void HistoryPanel::exportTo(const QString& path) {
  if (!haveView_ || historyJob_ != 0 || exportJob_ != 0) return;
  error_->clearError();
  exportJob_ = engine_.submit(QStringLiteral("history_export"), QJsonObject{{QStringLiteral("path"), path}});
  setBusy(true);
}

void HistoryPanel::showExport(const JobResult& result) {
  if (result.status == JobStatus::Ok) {
    status_->setText(QStringLiteral("Saved %1 bars to %2.")
                         .arg(whole(result.value[QStringLiteral("rows")].toInt()),
                              QDir::toNativeSeparators(result.value[QStringLiteral("path")].toString())));
  } else {
    showFailure(result);
  }
  setBusy(false);
}

void HistoryPanel::showFailure(const JobResult& result) {
  // These two are written for people, so the message stands alone with no technical prefix.
  const bool plain = result.errorType == QStringLiteral("BadRequest") || result.errorType == QStringLiteral("ProviderError");
  error_->showError(plain ? result.message : QStringLiteral("%1: %2").arg(result.errorType, result.message),
                    plain ? QString() : result.traceback);
}

}  // namespace rq
