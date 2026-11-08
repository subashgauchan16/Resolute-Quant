#include "app/TerminalPage.h"

#include <QApplication>
#include <QDateTime>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QShortcut>
#include <QStandardPaths>
#include <QTimeZone>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

#include "app/BrandMark.h"
#include "app/CandleChart.h"
#include "app/Format.h"
#include "app/Palette.h"
#include "app/TerminalWidgets.h"
#include "bridge/PythonEngine.h"

namespace rq {

namespace {

const QStringList kDefaultCrypto{"BTC-USD", "ETH-USD", "SOL-USD", "XRP-USD", "DOGE-USD", "ADA-USD", "LINK-USD", "AVAX-USD", "LTC-USD", "BTC-GBP"};
const QStringList kDefaultMarkets{"^GSPC", "^IXIC", "^FTSE", "AAPL", "NVDA", "MSFT", "TSLA", "HSBA.L", "GBPUSD=X", "GC=F"};
const QVector<std::pair<QString, int>> kCryptoTimeframes{{"1M", 60}, {"5M", 300}, {"15M", 900}, {"1H", 3600}, {"6H", 21600}, {"1D", 86400}};
const QVector<std::pair<QString, int>> kMarketTimeframes{{"1M", 60}, {"5M", 300}, {"15M", 900}, {"1H", 3600}, {"4H", 14400}, {"1D", 86400}, {"1W", 604800}, {"1MO", 2592000}};

constexpr int kMaxWatch = 40;
constexpr int kRepaintMs = 200;
constexpr int kStaleMs = 6000;
constexpr int kChartRetryMs = 15000;
constexpr int kMarketRefreshMs = 60000;
constexpr int kToastMs = 3500;

const auto kCommandPlaceholder = QStringLiteral("Type BTC, 5M, ADD PEPE, ALERT 77000 or HELP. Press / to focus.");
const QRegularExpression kPair(QStringLiteral("^[A-Z0-9]{1,12}-[A-Z0-9]{2,6}$"));
const QRegularExpression kMarketSymbol(QStringLiteral("^[A-Z0-9.^=\\-]{1,15}$"));

std::optional<double> numberIn(const QJsonObject& object, const char* key) {
  const QJsonValue value = object[QLatin1String(key)];
  return value.isDouble() ? std::optional<double>(value.toDouble()) : std::nullopt;
}

std::optional<double> changePercent(const std::optional<double>& last, const std::optional<double>& open) {
  if (!last || !open || *open == 0.0) return std::nullopt;
  return (*last / *open - 1.0) * 100.0;
}

QColor signColour(const std::optional<double>& value) {
  if (!value || *value == 0.0) return palette::kMute;
  return *value > 0.0 ? palette::kUp : palette::kDown;
}

int signOf(const std::optional<double>& value) { return !value || *value == 0.0 ? 0 : *value > 0.0 ? 1 : -1; }

QString normaliseCrypto(const QString& word) { return word.contains(QLatin1Char('-')) ? word : word + QStringLiteral("-USD"); }

std::pair<QString, QString> priceAffix(const QString& currency) {
  if (currency == QLatin1String("GBp")) return {QString(), QStringLiteral("p")};
  return {format::currencySymbol(currency), QString()};
}

QString colourName(const QColor& colour) { return colour.name(QColor::HexRgb); }

QLabel* styledLabel(const QFont& font, const QColor& colour = palette::kForeground) {
  auto* label = new QLabel;
  label->setFont(font);
  label->setStyleSheet(QStringLiteral("color: %1;").arg(colourName(colour)));
  return label;
}

QString ago(const QString& isoTime) {
  const QDateTime when = QDateTime::fromString(isoTime, Qt::ISODate);
  if (!when.isValid()) return {};
  const double minutes = when.msecsTo(QDateTime::currentDateTimeUtc()) / 60000.0;
  if (minutes < 60) return QStringLiteral("%1m ago").arg(std::max(1L, std::lround(minutes)));
  return minutes < 1440 ? QStringLiteral("%1h ago").arg(std::lround(minutes / 60)) : QStringLiteral("%1d ago").arg(std::lround(minutes / 1440));
}

}  // namespace

TerminalPage::TerminalPage(PythonEngine& engine, TerminalOptions options, QWidget* parent)
    : QWidget(parent),
      engine_(engine),
      options_(std::move(options)),
      startedMs_(QDateTime::currentMSecsSinceEpoch()),
      repaintTimer_(new QTimer(this)),
      retryTimer_(new QTimer(this)),
      refreshTimer_(new QTimer(this)),
      toastTimer_(new QTimer(this)) {
  setObjectName(QStringLiteral("terminal"));
  setFocusPolicy(Qt::StrongFocus);
  setAttribute(Qt::WA_StyledBackground);
  setStyleSheet(QStringLiteral("#terminal { background: #23262c; }"));

  const QString settingsFile = options_.settingsPath.isEmpty()
                                   ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/terminal.ini")
                                   : options_.settingsPath;
  settings_ = std::make_unique<QSettings>(settingsFile, QSettings::IniFormat);
  loadSettings();

  // The same grid as the page: 292px, the rest, 296px. Rows: 42px, alert, the rest, 84px, 22px, with 1px gaps.
  auto* grid = new QGridLayout(this);
  grid->setContentsMargins(0, 0, 0, 0);
  grid->setSpacing(1);
  grid->setColumnMinimumWidth(0, 292);
  grid->setColumnMinimumWidth(2, 296);
  grid->setColumnStretch(1, 1);
  grid->setRowStretch(2, 1);

  grid->addWidget(buildTopBar(), 0, 0, 1, 3);

  alertBar_ = new QWidget;
  alertBar_->setObjectName(QStringLiteral("alertBar"));
  alertBar_->setAttribute(Qt::WA_StyledBackground);
  alertBar_->setStyleSheet(QStringLiteral("#alertBar { background: #ffb000; } QLabel { color: #000000; font-weight: 600; }"));
  auto* alertLayout = new QHBoxLayout(alertBar_);
  alertLayout->setContentsMargins(12, 6, 12, 6);
  alertText_ = new QLabel;
  alertText_->setObjectName(QStringLiteral("alertText"));
  auto* dismiss = new QPushButton(QStringLiteral("Dismiss"));
  dismiss->setObjectName(QStringLiteral("linkButton"));
  dismiss->setCursor(Qt::PointingHandCursor);
  alertLayout->addWidget(alertText_, 1);
  alertLayout->addWidget(dismiss);
  alertBar_->hide();
  connect(dismiss, &QPushButton::clicked, alertBar_, &QWidget::hide);
  grid->addWidget(alertBar_, 1, 0, 1, 3);

  auto* watch = buildWatchlist();
  watch->setFixedWidth(292);
  grid->addWidget(watch, 2, 0);
  grid->addWidget(buildChartPanel(), 2, 1);
  auto* side = buildSidePanel();
  side->setFixedWidth(296);
  grid->addWidget(side, 2, 2);

  stats_ = new StatsStrip;
  grid->addWidget(stats_, 3, 0, 1, 3);
  grid->addWidget(buildFooter(), 4, 0, 1, 3);

  help_ = nullptr;
  intro_ = new LogoIntro(this);
  connect(brand_, &BrandMark::clicked, intro_, [this] { intro_->open(false); });

  watchlist_->setSymbols(markets_, crypto_);
  watchlist_->setSelected(selected_);
  if (!connected_ && !options_.connect) watchlist_->setNote(QStringLiteral("Offline. This session was started without a data connection."));
  applyMode();
  refreshAlertLines();

  createShortcuts();
  connect(&engine_, &PythonEngine::started, this, &TerminalPage::onEngineStarted);
  connect(&engine_, &PythonEngine::startFailed, this, &TerminalPage::onEngineFailed);
  connect(&engine_, &PythonEngine::jobFinished, this, &TerminalPage::onJobFinished);
  connect(&engine_, &PythonEngine::pythonEvent, this, &TerminalPage::onEvent);
  connect(chart_, &CandleChart::hoverChanged, this, [this](int index) { paintLegend(index); });

  connect(repaintTimer_, &QTimer::timeout, this, &TerminalPage::repaintAll);
  repaintTimer_->start(kRepaintMs);
  retryTimer_->setSingleShot(true);
  connect(retryTimer_, &QTimer::timeout, this, [this] { loadChart(true); });
  refreshTimer_->setInterval(kMarketRefreshMs);
  connect(refreshTimer_, &QTimer::timeout, this, [this] {
    if (isMarket(selected_) && connected_) loadChart(true);
  });
  refreshTimer_->start();
  toastTimer_->setSingleShot(true);
  connect(toastTimer_, &QTimer::timeout, this, [this] {
    command_->setPlaceholderText(kCommandPlaceholder);
    commandBox_->setStyleSheet(QStringLiteral("#commandBox { background: #07080a; border: 1px solid #23262c; }"));
  });

  repaintAll();
  if (options_.showIntro) QTimer::singleShot(0, this, [this] { intro_->open(true); });
}

void TerminalPage::resizeEvent(QResizeEvent*) {
  if (intro_->isVisible()) intro_->setGeometry(rect());
  if (help_ != nullptr) help_->setGeometry(rect());
}

bool TerminalPage::eventFilter(QObject* watched, QEvent* event) {
  if (watched == help_ && event->type() == QEvent::MouseButtonPress) help_->hide();
  return QWidget::eventFilter(watched, event);
}

QWidget* TerminalPage::buildTopBar() {
  auto* bar = new QWidget;
  bar->setFixedHeight(42);
  auto* layout = new QHBoxLayout(bar);
  layout->setContentsMargins(12, 0, 12, 0);
  layout->setSpacing(14);

  brand_ = new BrandMark;
  layout->addWidget(brand_);

  commandBox_ = new QWidget;
  commandBox_->setObjectName(QStringLiteral("commandBox"));
  commandBox_->setAttribute(Qt::WA_StyledBackground);
  commandBox_->setFixedHeight(28);
  commandBox_->setStyleSheet(QStringLiteral("#commandBox { background: #07080a; border: 1px solid #23262c; }"));
  auto* box = new QHBoxLayout(commandBox_);
  box->setContentsMargins(9, 0, 9, 0);
  box->setSpacing(8);
  auto* prompt = styledLabel(palette::mono(13), palette::kAmber);
  prompt->setText(QStringLiteral(">"));
  command_ = new QLineEdit;
  command_->setObjectName(QStringLiteral("command"));
  command_->setPlaceholderText(kCommandPlaceholder);
  command_->setFrame(false);
  command_->setStyleSheet(QStringLiteral("QLineEdit { background: transparent; border: none; padding: 0; }"));
  command_->setAccessibleName(QStringLiteral("Command"));
  command_->setFocusPolicy(Qt::ClickFocus);  // Focus comes from a click or the / key, as in the browser.
  connect(command_, &QLineEdit::textEdited, this, [this](const QString& text) {
    const int cursor = command_->cursorPosition();
    command_->setText(text.toUpper());
    command_->setCursorPosition(cursor);
  });
  connect(command_, &QLineEdit::returnPressed, this, [this] {
    const QString text = command_->text();
    command_->clear();
    runCommand(text);
  });
  box->addWidget(prompt);
  box->addWidget(command_, 1);
  layout->addWidget(commandBox_, 1);

  const auto badge = [&](QLabel*& dot, QLabel*& text, const char* name, int size, const QString& initial) {
    auto* holder = new QWidget;
    auto* row = new QHBoxLayout(holder);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(7);
    dot = new QLabel;
    dot->setFixedSize(8, 8);
    text = styledLabel(palette::mono(size, 500));
    text->setObjectName(QString::fromLatin1(name));
    text->setText(initial);
    row->addWidget(dot);
    row->addWidget(text);
    layout->addWidget(holder);
  };
  badge(cryptoDot_, cryptoStatus_, "cryptoStatus", 13, QStringLiteral("CONNECTING"));
  badge(yahooDot_, yahooStatus_, "yahooStatus", 12, QStringLiteral("STOCKS OFF"));

  meta_ = styledLabel(palette::mono(13), palette::kMute);
  meta_->setObjectName(QStringLiteral("meta"));
  meta_->setToolTip(QStringLiteral("Messages received per second, and the time since each message was stamped by the exchange"));
  layout->addWidget(meta_);
  clock_ = styledLabel(palette::mono(13));
  clock_->setObjectName(QStringLiteral("clock"));
  layout->addWidget(clock_);

  const auto iconButton = [&](const QString& text, const QString& tip) {
    auto* button = new QPushButton(text);
    button->setFixedHeight(28);
    button->setCursor(Qt::PointingHandCursor);
    button->setToolTip(tip);
    button->setFocusPolicy(Qt::TabFocus);
    layout->addWidget(button);
    return button;
  };
  historyButton_ = iconButton(QStringLiteral("HISTORY"), QStringLiteral("Price history and CSV export for the open symbol"));
  historyButton_->setObjectName(QStringLiteral("historyButton"));
  fullButton_ = iconButton(QStringLiteral("FULL"), QStringLiteral("Full screen"));
  helpButton_ = iconButton(QStringLiteral("HELP"), QString());
  helpButton_->setObjectName(QStringLiteral("helpButton"));
  connect(historyButton_, &QPushButton::clicked, this, [this] { emit historyRequested(selected_, isMarket(selected_)); });
  connect(fullButton_, &QPushButton::clicked, this, [this] {
    window()->isFullScreen() ? window()->showNormal() : window()->showFullScreen();
  });
  connect(helpButton_, &QPushButton::clicked, this, &TerminalPage::showHelp);
  return bar;
}

QWidget* TerminalPage::buildWatchlist() {
  auto* panel = new QWidget;
  auto* layout = new QVBoxLayout(panel);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  layout->addWidget(new PanelHead(QStringLiteral("WATCHLIST"), QStringLiteral("CHANGE")));
  watchlist_ = new WatchlistView;
  auto* scroll = new QScrollArea;
  scroll->setWidget(watchlist_);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  layout->addWidget(scroll, 1);
  connect(watchlist_, &WatchlistView::symbolClicked, this, &TerminalPage::select);
  return panel;
}

QWidget* TerminalPage::buildChartPanel() {
  auto* panel = new QWidget;
  auto* layout = new QVBoxLayout(panel);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  auto* quoteRow = new QWidget;
  auto* quoteLayout = new QHBoxLayout(quoteRow);
  quoteLayout->setContentsMargins(12, 8, 12, 6);
  quoteLayout->setSpacing(14);
  pairLabel_ = styledLabel(palette::label(20, 600, 0.04));
  pairLabel_->setObjectName(QStringLiteral("pair"));
  bigPrice_ = styledLabel(palette::mono(26, 600));
  bigPrice_->setObjectName(QStringLiteral("bigPrice"));
  bigChange_ = styledLabel(palette::mono(13), palette::kMute);
  bigChange_->setObjectName(QStringLiteral("bigChange"));
  asOf_ = styledLabel(palette::mono(12), palette::kMute);
  asOf_->setObjectName(QStringLiteral("asOf"));
  // This note gives way first when space is short, so it can never squash the timeframe buttons.
  asOf_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  quoteLayout->addWidget(pairLabel_, 0, Qt::AlignBaseline);
  quoteLayout->addWidget(bigPrice_, 0, Qt::AlignBaseline);
  quoteLayout->addWidget(bigChange_, 0, Qt::AlignBaseline);
  quoteLayout->addWidget(asOf_, 1, Qt::AlignBaseline);
  auto* buttons = new QWidget;
  buttons->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
  timeframeLayout_ = new QHBoxLayout(buttons);
  timeframeLayout_->setContentsMargins(0, 0, 0, 0);
  timeframeLayout_->setSpacing(2);
  quoteLayout->addWidget(buttons, 0, Qt::AlignVCenter);
  layout->addWidget(quoteRow);

  legend_ = new QLabel;
  legend_->setObjectName(QStringLiteral("legend"));
  legend_->setFont(palette::mono(12));
  legend_->setContentsMargins(12, 0, 12, 5);
  legend_->setTextFormat(Qt::RichText);
  layout->addWidget(legend_);

  chart_ = new CandleChart;
  chart_->setObjectName(QStringLiteral("chart"));
  chart_->setTerminalStyle(true);
  layout->addWidget(chart_, 1);
  return panel;
}

QWidget* TerminalPage::buildSidePanel() {
  auto* side = new QWidget;
  side->setObjectName(QStringLiteral("side"));
  side->setAttribute(Qt::WA_StyledBackground);
  side->setStyleSheet(QStringLiteral("#side { background: #23262c; }"));
  auto* layout = new QVBoxLayout(side);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(1);

  bookPane_ = new QWidget;
  auto* bookLayout = new QVBoxLayout(bookPane_);
  bookLayout->setContentsMargins(0, 0, 0, 0);
  bookLayout->setSpacing(0);
  bookHead_ = new PanelHead(QStringLiteral("ORDER BOOK"), QStringLiteral("WAITING"));
  bookHead_->setObjectName(QStringLiteral("bookHead"));
  book_ = new OrderBookView;
  bookLayout->addWidget(bookHead_);
  bookLayout->addWidget(book_);
  layout->addWidget(bookPane_);

  tapePane_ = new QWidget;
  auto* tapeLayout = new QVBoxLayout(tapePane_);
  tapeLayout->setContentsMargins(0, 0, 0, 0);
  tapeLayout->setSpacing(0);
  tapeLayout->addWidget(new PanelHead(QStringLiteral("TIME & SALES"), QStringLiteral("LIVE TRADES")));
  tape_ = new TapeView;
  tapeLayout->addWidget(tape_, 1);
  layout->addWidget(tapePane_, 1);

  const auto scrolled = [](QLabel* label) {
    label->setWordWrap(true);
    label->setTextFormat(Qt::RichText);
    label->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    label->setContentsMargins(10, 6, 10, 8);
    auto* scroll = new QScrollArea;
    scroll->setWidget(label);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    return scroll;
  };

  profilePane_ = new QWidget;
  auto* profileLayout = new QVBoxLayout(profilePane_);
  profileLayout->setContentsMargins(0, 0, 0, 0);
  profileLayout->setSpacing(0);
  profileLayout->addWidget(new PanelHead(QStringLiteral("PROFILE"), QStringLiteral("YAHOO FINANCE")));
  profileText_ = new QLabel;
  profileText_->setObjectName(QStringLiteral("profile"));
  // As tall as its content, as in the browser. News takes what is left.
  profileText_->setWordWrap(true);
  profileText_->setTextFormat(Qt::RichText);
  profileText_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
  profileText_->setContentsMargins(10, 6, 10, 8);
  profileLayout->addWidget(profileText_);
  layout->addWidget(profilePane_);

  newsPane_ = new QWidget;
  auto* newsLayout = new QVBoxLayout(newsPane_);
  newsLayout->setContentsMargins(0, 0, 0, 0);
  newsLayout->setSpacing(0);
  newsHead_ = new PanelHead(QStringLiteral("NEWS"), QStringLiteral("YAHOO FINANCE"));
  newsLayout->addWidget(newsHead_);
  newsText_ = new QLabel;
  newsText_->setObjectName(QStringLiteral("news"));
  newsText_->setOpenExternalLinks(true);
  newsLayout->addWidget(scrolled(newsText_), 1);
  layout->addWidget(newsPane_, 1);
  return side;
}

QWidget* TerminalPage::buildFooter() {
  auto* footer = new QWidget;
  footer->setFixedHeight(22);
  auto* layout = new QHBoxLayout(footer);
  layout->setContentsMargins(12, 0, 12, 0);
  layout->setSpacing(14);
  auto* copyright = new QLabel(QStringLiteral("<a style=\"color:#e9e6df;text-decoration:none\" href=\"https://github.com/subashgauchan16/Resolute-Quant\">© 2026 RESOLUTE QUANT | SG | subashgauchan16</a>"));
  copyright->setFont(palette::mono(11));
  copyright->setOpenExternalLinks(true);
  auto* sources = styledLabel(palette::mono(11), palette::kMute);
  sources->setText(QStringLiteral("All data through the local Python engine. Crypto: Coinbase Exchange. Stocks, indices and FX: Yahoo Finance via yfinance, personal use, may be delayed. Fear & Greed: alternative.me. Places no orders."));
  sources->setMinimumWidth(0);
  sources->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  session_label_ = styledLabel(palette::mono(11), palette::kMute);
  session_label_->setText(QStringLiteral("Session started %1").arg(QDateTime::fromMSecsSinceEpoch(startedMs_).toString(QStringLiteral("HH:mm"))));
  layout->addWidget(copyright);
  layout->addWidget(sources, 1);
  layout->addWidget(session_label_);
  return footer;
}

void TerminalPage::createShortcuts() {
  const auto shortcut = [this](const QKeySequence& keys, auto handler) {
    auto* item = new QShortcut(keys, this);
    item->setContext(Qt::WidgetWithChildrenShortcut);
    connect(item, &QShortcut::activated, this, handler);
  };
  shortcut(QKeySequence(Qt::Key_Slash), [this] { command_->setFocus(); });
  shortcut(QKeySequence(Qt::Key_F), [this] { window()->isFullScreen() ? window()->showNormal() : window()->showFullScreen(); });
  const auto move = [this](int direction) {
    const QStringList all = markets_ + crypto_;
    const int index = static_cast<int>(all.indexOf(selected_));
    select(all[(index + direction + all.size()) % all.size()]);
  };
  shortcut(QKeySequence(Qt::Key_Down), [move] { move(1); });
  shortcut(QKeySequence(Qt::Key_Up), [move] { move(-1); });
  shortcut(QKeySequence(Qt::Key_Escape), [this] {
    if (help_ != nullptr) help_->hide();
    command_->clear();
    if (command_->hasFocus()) setFocus();
  });
}

void TerminalPage::showHelp() {
  if (help_ == nullptr) {
    help_ = new QWidget(this);
    help_->setObjectName(QStringLiteral("help"));
    help_->setAttribute(Qt::WA_StyledBackground);
    help_->setStyleSheet(QStringLiteral("#help { background: rgba(0,0,0,184); } #helpBox { background: #07080a; border: 1px solid #ffb000; }"));
    auto* outer = new QVBoxLayout(help_);
    auto* box = new QWidget;
    box->setObjectName(QStringLiteral("helpBox"));
    box->setAttribute(Qt::WA_StyledBackground);
    box->setMaximumWidth(560);
    auto* inner = new QVBoxLayout(box);
    inner->setContentsMargins(18, 16, 18, 16);
    auto* title = styledLabel(palette::label(15, 600, 0.1), palette::kAmber);
    title->setText(QStringLiteral("COMMANDS"));
    inner->addWidget(title);
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(6);
    const QVector<std::pair<QString, QString>> commands{
        {"BTC or AAPL or VOD.L", "Open a symbol. Coinbase is checked first, then Yahoo."},
        {"STK META", "Use Yahoo when the symbol exists on both."},
        {"1M 5M 15M 1H 1D", "Change the candle size. 1M is one minute. Crypto adds 6H. Markets add 4H, 1W and 1MO (one month)."},
        {"HISTORY", "Show past bars for the open symbol, and export them as CSV."},
        {"ADD PEPE", "Add a symbol to the watchlist."},
        {"DEL PEPE", "Remove a symbol."},
        {"ALERT 77000", "Alert when the open symbol crosses a price."},
        {"CLEAR", "Remove alerts on the open symbol."},
        {"HEALTH", "Show the embedded Python's health."},
        {"/ · ↑ ↓ · F · Esc", "Focus command, move through the list, full screen, close."},
    };
    for (int i = 0; i < commands.size(); ++i) {
      auto* key = styledLabel(palette::mono(13), palette::kAmber);
      key->setText(commands[i].first);
      key->setAlignment(Qt::AlignTop | Qt::AlignLeft);
      auto* value = styledLabel(palette::mono(13));
      value->setText(commands[i].second);
      value->setWordWrap(true);
      grid->addWidget(key, i, 0);
      grid->addWidget(value, i, 1);
    }
    grid->setColumnStretch(1, 1);
    inner->addLayout(grid);
    auto* note = styledLabel(palette::mono(13), palette::kMute);
    note->setText(QStringLiteral("Your watchlist and alerts are saved on this PC. Alerts only fire while the app is open."));
    note->setWordWrap(true);
    note->setContentsMargins(0, 8, 0, 0);
    inner->addWidget(note);
    outer->addStretch();
    outer->addWidget(box, 0, Qt::AlignHCenter);
    outer->addStretch();
    help_->installEventFilter(this);
    auto* close = new QShortcut(QKeySequence(Qt::Key_Escape), help_);
    connect(close, &QShortcut::activated, help_, &QWidget::hide);
  }
  help_->setGeometry(rect());
  help_->raise();
  help_->show();
  help_->setFocus();
}

void TerminalPage::loadSettings() {
  const auto load = [this](const char* key, const QStringList& fallback, const QRegularExpression& valid) {
    QStringList list = settings_->value(QLatin1String(key), fallback).toStringList();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const QString& s) { return !valid.match(s).hasMatch(); }), list.end());
    return list;
  };
  markets_ = load("markets", kDefaultMarkets, kMarketSymbol);
  crypto_ = load("crypto", kDefaultCrypto, kPair);
  if (crypto_.isEmpty()) crypto_ = kDefaultCrypto;
  selected_ = settings_->value(QStringLiteral("selected"), QStringLiteral("BTC-USD")).toString();
  if (!crypto_.contains(selected_) && !markets_.contains(selected_)) selected_ = crypto_.first();
  granularity_ = settings_->value(QStringLiteral("granularity"), 300).toInt();
  const auto frames = timeframes();
  if (std::none_of(frames.cbegin(), frames.cend(), [this](const auto& f) { return f.second == granularity_; })) granularity_ = 300;
  for (const QString& entry : settings_->value(QStringLiteral("alerts")).toStringList()) {
    const QString symbol = entry.section(QLatin1Char('|'), 0, 0);
    bool ok = false;
    const double price = entry.section(QLatin1Char('|'), 1).toDouble(&ok);
    if (ok && price > 0.0) alerts_.push_back({symbol, price});
  }
}

void TerminalPage::saveSettings() {
  settings_->setValue(QStringLiteral("markets"), markets_);
  settings_->setValue(QStringLiteral("crypto"), crypto_);
  settings_->setValue(QStringLiteral("selected"), selected_);
  settings_->setValue(QStringLiteral("granularity"), granularity_);
  QStringList saved;
  for (const Alert& alert : alerts_) saved << QStringLiteral("%1|%2").arg(alert.symbol).arg(alert.price, 0, 'g', 17);
  settings_->setValue(QStringLiteral("alerts"), saved);
}

QVector<std::pair<QString, int>> TerminalPage::timeframes() const { return isMarket(selected_) ? kMarketTimeframes : kCryptoTimeframes; }

QString TerminalPage::timeframe() const {
  for (const auto& frame : timeframes()) {
    if (frame.second == granularity_) return frame.first;
  }
  return QStringLiteral("5M");
}

void TerminalPage::rebuildTimeframes() {
  qDeleteAll(timeframeButtons_);
  timeframeButtons_.clear();
  for (const auto& frame : timeframes()) {
    auto* button = new QPushButton(format::timeframeLabel(frame.first));
    button->setObjectName(QStringLiteral("interval"));
    button->setCheckable(true);
    button->setChecked(frame.second == granularity_);
    button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::TabFocus);
    const QString name = frame.first;
    connect(button, &QPushButton::clicked, this, [this, name] { setTimeframe(name); });
    timeframeLayout_->addWidget(button);
    timeframeButtons_.push_back(button);
  }
}

void TerminalPage::applyMode() {
  const bool market = isMarket(selected_);
  bookPane_->setVisible(!market);
  tapePane_->setVisible(!market);
  profilePane_->setVisible(market);
  newsPane_->setVisible(market);
  const auto frames = timeframes();
  if (std::none_of(frames.cbegin(), frames.cend(), [this](const auto& f) { return f.second == granularity_; })) granularity_ = 300;
  rebuildTimeframes();
  pairLabel_->setText(market ? format::friendlyName(selected_) : selected_);
}

void TerminalPage::select(const QString& symbol) {
  if (symbol == selected_ && chartJob_ == 0 && chart_->candleCount() > 0) return;
  selected_ = symbol;
  saveSettings();
  profile_ = {};
  session_ = {};
  bookReady_ = false;
  book_->clearBook();
  tape_->setTrades({});
  quote(symbol).dirty = true;
  applyMode();
  watchlist_->setSelected(symbol);
  refreshAlertLines();
  if (connected_) engine_.submit(QStringLiteral("terminal_select"), QJsonObject{{QStringLiteral("crypto"), isMarket(symbol) ? QString() : symbol}});
  loadChart();
  loadDetails();
  repaintAll();
}

void TerminalPage::setTimeframe(const QString& name) {
  for (const auto& frame : timeframes()) {
    if (frame.first != name) continue;
    granularity_ = frame.second;
    saveSettings();
    rebuildTimeframes();
    loadChart();
    return;
  }
}

void TerminalPage::loadChart(bool keepView) {
  const QString interval = timeframe();
  retryTimer_->stop();
  // A symbol's candles must never stay on screen under another symbol's name.
  if (selected_ != chartSymbol_ || interval != chartInterval_) {
    chartSymbol_ = selected_;
    chartInterval_ = interval;
    chart_->clear();
    paintLegend();
  }
  chart_->setPriceDecimals(-1);
  if (!connected_) {
    if (!options_.connect) chart_->clear(QStringLiteral("Offline. This session was started without a data connection."));
    return;
  }
  chartJob_ = engine_.submit(isMarket(selected_) ? QStringLiteral("market_chart") : QStringLiteral("live_chart"),
                             QJsonObject{{QStringLiteral("symbol"), selected_}, {QStringLiteral("interval"), interval}, {QStringLiteral("keepView"), keepView}});
}

void TerminalPage::showChart(const JobResult& result) {
  if (result.status != JobStatus::Ok) {
    const QString text = chart_->candleCount() > 0
                             ? QStringLiteral("Could not update %1. Showing the last bars loaded, which may be out of date. Retrying in 15 seconds.").arg(selected_)
                             : QStringLiteral("Chart history for %1 did not load. Retrying in 15 seconds.").arg(selected_);
    chart_->setMessage(text);
    retryTimer_->start(kChartRetryMs);
    return;
  }
  const QString symbol = result.value[QStringLiteral("symbol")].toString();
  const QString interval = result.value[QStringLiteral("interval")].toString();
  if (symbol != chartSymbol_ || interval != chartInterval_) return;  // The person has moved on.
  QVector<Candle> candles;
  const QJsonArray bars = result.value[QStringLiteral("bars")].toArray();
  candles.reserve(bars.size());
  for (const auto& entry : bars) {
    const QJsonObject bar = entry.toObject();
    Candle candle;
    candle.time = bar[QStringLiteral("time")].toInteger();
    candle.open = bar[QStringLiteral("open")].toDouble();
    candle.high = bar[QStringLiteral("high")].toDouble();
    candle.low = bar[QStringLiteral("low")].toDouble();
    candle.close = bar[QStringLiteral("close")].toDouble();
    candle.hasVolume = bar[QStringLiteral("volume")].isDouble();
    candle.volume = bar[QStringLiteral("volume")].toDouble();
    candles.push_back(candle);
  }
  if (!candles.isEmpty()) chart_->setPriceDecimals(format::decimalsForSymbol(isMarket(symbol), symbol, candles.last().close));
  const bool longBars = interval == QLatin1String("1D") || interval == QLatin1String("1W") || interval == QLatin1String("1MO");
  chart_->setMessage(QString());
  chart_->setCandles(std::move(candles), !longBars, chart_->candleCount() > 0);
  paintLegend();
}

void TerminalPage::loadDetails() {
  if (!isMarket(selected_)) return;
  profileText_->setText(QStringLiteral("<span style=\"color:#8d9099;font-size:12px\">Loading %1.</span>").arg(format::friendlyName(selected_).toHtmlEscaped()));
  newsText_->clear();
  newsHead_->setNote(QStringLiteral("YAHOO FINANCE"));
  if (!connected_) {
    profileText_->setText(QStringLiteral("<span style=\"color:#8d9099;font-size:12px\">Offline.</span>"));
    return;
  }
  profileJob_ = engine_.submit(QStringLiteral("market_profile"), QJsonObject{{QStringLiteral("symbol"), selected_}});
  newsJob_ = engine_.submit(QStringLiteral("market_news"), QJsonObject{{QStringLiteral("symbol"), selected_}});
}

void TerminalPage::showProfile(const JobResult& result) {
  if (result.status != JobStatus::Ok) {
    profileText_->setText(QStringLiteral("<span style=\"color:#8d9099;font-size:12px\">%1</span>").arg(result.message.toHtmlEscaped()));
    return;
  }
  if (result.value[QStringLiteral("symbol")].toString() != selected_) return;
  profile_ = result.value[QStringLiteral("profile")].toObject();
  const QJsonObject& p = profile_;
  const QString name = p[QStringLiteral("name")].toString(format::friendlyName(selected_));
  const QString exchange = p[QStringLiteral("exchange")].toString();
  QString html = QStringLiteral("<div style=\"font-family:'IBM Plex Sans Condensed';font-weight:600;font-size:15px;color:#e9e6df\">%1</div>"
                                "<div style=\"color:#8d9099;font-size:12px;margin-bottom:7px\">%2%3</div><table width=\"100%\" cellspacing=\"0\" cellpadding=\"2\">")
                     .arg(name.toHtmlEscaped(), selected_.toHtmlEscaped(), exchange.isEmpty() ? QString() : QStringLiteral(" · ") + exchange.toHtmlEscaped());
  const auto row = [&html](const QString& key, const QString& value) {
    if (value.isEmpty()) return;
    html += QStringLiteral("<tr><td style=\"color:#8d9099;font-size:12px;border-bottom:1px solid #15171b\">%1</td><td align=\"right\" style=\"font-size:12px;font-weight:600;border-bottom:1px solid #15171b\">%2</td></tr>").arg(key, value.toHtmlEscaped());
  };
  const auto number = [&p](const char* key, int decimals) {
    const auto value = numberIn(p, key);
    return value ? format::number(value, decimals) : QString();
  };
  row(QStringLiteral("Sector"), p[QStringLiteral("sector")].toString());
  row(QStringLiteral("Industry"), p[QStringLiteral("industry")].toString());
  row(QStringLiteral("Country"), p[QStringLiteral("country")].toString());
  row(QStringLiteral("Forward P/E"), number("forwardPe", 1));
  row(QStringLiteral("EPS"), number("eps", 2));
  const QString dividend = number("dividend", 2);
  row(QStringLiteral("Dividend yield"), dividend.isEmpty() ? QString() : dividend + QLatin1Char('%'));
  row(QStringLiteral("Beta"), number("beta", 2));
  const auto average = numberIn(p, "avgVolume");
  row(QStringLiteral("Average volume"), average ? format::compact(average) : QString());
  html += QStringLiteral("</table>");
  profileText_->setText(html);
  paintMarketStats();
}

void TerminalPage::showNews(const JobResult& result) {
  if (result.status != JobStatus::Ok) {
    newsText_->setText(QStringLiteral("<span style=\"color:#8d9099;font-size:12px\">%1</span>").arg(result.message.toHtmlEscaped()));
    return;
  }
  if (result.value[QStringLiteral("symbol")].toString() != selected_) return;
  const QJsonArray items = result.value[QStringLiteral("items")].toArray();
  if (items.isEmpty()) {
    newsText_->setText(QStringLiteral("<span style=\"color:#8d9099;font-size:12px\">No recent stories for %1.</span>").arg(format::friendlyName(selected_).toHtmlEscaped()));
    return;
  }
  QString html;
  for (const auto& entry : items) {
    const QJsonObject item = entry.toObject();
    html += QStringLiteral("<p style=\"margin:0 0 9px 0\"><a style=\"color:#e9e6df;text-decoration:none;font-family:'IBM Plex Sans Condensed';font-weight:500;font-size:13px\" href=\"%1\">%2</a><br>"
                           "<span style=\"color:#8d9099;font-size:11px\">%3 · %4</span></p>")
                .arg(item[QStringLiteral("url")].toString().toHtmlEscaped(), item[QStringLiteral("title")].toString().toHtmlEscaped(),
                     item[QStringLiteral("publisher")].toString().toHtmlEscaped(), ago(item[QStringLiteral("time")].toString()));
  }
  newsText_->setText(html);
}

void TerminalPage::onEngineStarted() {
  engineRunning_ = true;
  if (!options_.connect) {
    loadChart();
    loadDetails();
    return;
  }
  connected_ = true;
  startJob_ = engine_.submit(options_.startJob, QJsonObject{{QStringLiteral("crypto"), QJsonArray::fromStringList(crypto_)},
                                                            {QStringLiteral("markets"), QJsonArray::fromStringList(markets_)},
                                                            {QStringLiteral("selected"), isMarket(selected_) ? QString() : selected_}});
  loadChart();
  loadDetails();
}

void TerminalPage::onEngineFailed(const QString& reason) {
  watchlist_->setNote(QStringLiteral("The data engine could not start."));
  chart_->clear(reason);
}

void TerminalPage::pushWatch() {
  if (connected_) {
    engine_.submit(QStringLiteral("terminal_watch"), QJsonObject{{QStringLiteral("crypto"), QJsonArray::fromStringList(crypto_)},
                                                                 {QStringLiteral("markets"), QJsonArray::fromStringList(markets_)}});
  }
}

void TerminalPage::onJobFinished(const JobResult& result) {
  if (result.id == startJob_) {
    startJob_ = 0;
    if (result.status != JobStatus::Ok) showAlert(QStringLiteral("The data feed did not start: %1").arg(result.message));
  } else if (result.id == chartJob_) {
    chartJob_ = 0;
    showChart(result);
  } else if (result.id == profileJob_) {
    profileJob_ = 0;
    showProfile(result);
  } else if (result.id == newsJob_) {
    newsJob_ = 0;
    showNews(result);
  } else if (resolveJobs_.contains(result.id)) {
    const QString failure = resolveJobs_.take(result.id);
    if (result.status == JobStatus::Ok) {
      addResolved(result.value[QStringLiteral("kind")].toString(), result.value[QStringLiteral("symbol")].toString());
    } else {
      toast(result.errorType == QLatin1String("BadRequest") || result.errorType == QLatin1String("ProviderError") ? result.message : failure, true);
    }
  }
}

void TerminalPage::onEvent(const QJsonObject& message) {
  if (!connected_) return;
  const QString type = message[QStringLiteral("type")].toString();
  if (type == QLatin1String("live")) onLiveEvent(message);
  else if (type == QLatin1String("markets")) onMarketsEvent(message);
}

void TerminalPage::onLiveEvent(const QJsonObject& message) {
  const double now = static_cast<double>(QDateTime::currentMSecsSinceEpoch());
  if (message.contains(QStringLiteral("status"))) {
    const QJsonObject status = message[QStringLiteral("status")].toObject();
    feedUp_ = status[QStringLiteral("connected")].toBool();
    hasFeedStatus_ = true;
    retryAtMs_ = now + status[QStringLiteral("retryIn")].toDouble() * 1000.0;
    lastMessageMs_ = std::max(lastMessageMs_, status[QStringLiteral("lastMessage")].toDouble());
  }
  if (message.contains(QStringLiteral("seed"))) {
    const QJsonObject seed = message[QStringLiteral("seed")].toObject();
    Quote& q = quote(seed[QStringLiteral("id")].toString());
    if (!q.open) q.open = numberIn(seed, "open");
    if (auto high = numberIn(seed, "high")) q.high = high;
    if (auto low = numberIn(seed, "low")) q.low = low;
    if (auto volume = numberIn(seed, "volume")) q.volume = volume;
    if (!q.last) q.last = numberIn(seed, "last");
    if (seed.contains(QStringLiteral("spark"))) {
      q.spark.clear();
      for (const auto& point : seed[QStringLiteral("spark")].toArray()) q.spark.push_back(point.toDouble());
    }
    q.dirty = true;
  }
  if (message.contains(QStringLiteral("fearGreed"))) {
    const QJsonValue value = message[QStringLiteral("fearGreed")];
    if (value.isObject()) {
      fearGreedValue_ = value.toObject()[QStringLiteral("value")].toInt();
      fearGreedNote_ = value.toObject()[QStringLiteral("classification")].toString() + QStringLiteral(", daily");
    } else {
      fearGreedNote_ = QStringLiteral("Could not load. Retries hourly.");
    }
  }
  for (const auto& entry : message[QStringLiteral("tickers")].toArray()) {
    const QJsonObject ticker = entry.toObject();
    const QString symbol = ticker[QStringLiteral("id")].toString();
    Quote& q = quote(symbol);
    const std::optional<double> before = q.last;
    const double price = ticker[QStringLiteral("price")].toDouble();
    if (auto open = numberIn(ticker, "open")) q.open = open;
    if (auto high = numberIn(ticker, "high")) q.high = high;
    if (auto low = numberIn(ticker, "low")) q.low = low;
    if (auto volume = numberIn(ticker, "vol")) q.volume = volume;
    q.last = price;
    q.flash = !before || *before == price ? q.flash : price > *before ? 1 : -1;
    q.dirty = true;
    lastMessageMs_ = now;
    everLive_ = true;
    checkAlerts(symbol, before, price);
  }
  if (message.contains(QStringLiteral("bar"))) {
    const QJsonObject bar = message[QStringLiteral("bar")].toObject();
    if (bar[QStringLiteral("symbol")].toString() == chartSymbol_ && bar[QStringLiteral("interval")].toString() == chartInterval_) {
      Candle candle;
      candle.time = bar[QStringLiteral("time")].toInteger();
      candle.open = bar[QStringLiteral("open")].toDouble();
      candle.high = bar[QStringLiteral("high")].toDouble();
      candle.low = bar[QStringLiteral("low")].toDouble();
      candle.close = bar[QStringLiteral("close")].toDouble();
      candle.hasVolume = bar[QStringLiteral("volume")].isDouble();
      candle.volume = bar[QStringLiteral("volume")].toDouble();
      chart_->updateLatest(candle);
      if (chart_->hoveredIndex() < 0) paintLegend();
    }
  }
  if (message.contains(QStringLiteral("book"))) {
    const QJsonObject bookMessage = message[QStringLiteral("book")].toObject();
    if (bookMessage[QStringLiteral("id")].toString() == selected_) {
      book_->setBook(bookMessage[QStringLiteral("bids")].toArray(), bookMessage[QStringLiteral("asks")].toArray());
      bookReady_ = true;
      bookDepth_ = bookMessage[QStringLiteral("depth")].toInt();
      lastMessageMs_ = now;
    }
  }
  if (message.contains(QStringLiteral("session"))) {
    const QJsonObject session = message[QStringLiteral("session")].toObject();
    if (session[QStringLiteral("id")].toString() == selected_) {
      session_ = session;
      tape_->setTrades(session[QStringLiteral("tape")].toArray());
    }
  }
  if (message.contains(QStringLiteral("rate"))) {
    const QJsonObject rate = message[QStringLiteral("rate")].toObject();
    const QJsonValue lag = rate[QStringLiteral("lag")];
    meta_->setText(QStringLiteral("%1 msg/s · %2").arg(rate[QStringLiteral("perSecond")].toInt()).arg(lag.isDouble() ? QStringLiteral("%1 ms").arg(std::lround(lag.toDouble())) : QStringLiteral("-")));
  }
  if (message[QStringLiteral("restored")].toBool() && !isMarket(selected_)) loadChart(true);  // Trades were missed while disconnected.
}

void TerminalPage::onMarketsEvent(const QJsonObject& message) {
  if (message.contains(QStringLiteral("status"))) {
    yahooUp_ = true;
    yahooLimited_ = message[QStringLiteral("status")].toObject()[QStringLiteral("limited")].toBool();
  }
  if (message.contains(QStringLiteral("quote"))) {
    yahooUp_ = true;
    const QJsonObject m = message[QStringLiteral("quote")].toObject();
    const QString symbol = m[QStringLiteral("id")].toString();
    Quote& q = quote(symbol);
    const std::optional<double> before = q.last;
    if (auto price = numberIn(m, "price")) {
      q.flash = !before || *before == *price ? q.flash : *price > *before ? 1 : -1;
      q.last = price;
      checkAlerts(symbol, before, *price);
    }
    if (auto prev = numberIn(m, "prev")) q.open = prev;
    if (auto high = numberIn(m, "high")) q.high = high;
    if (auto low = numberIn(m, "low")) q.low = low;
    if (auto volume = numberIn(m, "vol")) q.volume = volume;
    if (auto cap = numberIn(m, "cap")) q.cap = cap;
    if (auto yearHigh = numberIn(m, "yearHigh")) q.yearHigh = yearHigh;
    if (auto yearLow = numberIn(m, "yearLow")) q.yearLow = yearLow;
    if (auto delay = numberIn(m, "delay")) q.delay = delay;
    if (m.contains(QStringLiteral("cur"))) q.currency = m[QStringLiteral("cur")].toString();
    if (m.contains(QStringLiteral("state"))) q.state = m[QStringLiteral("state")].toString();
    if (m.contains(QStringLiteral("name"))) q.name = m[QStringLiteral("name")].toString();
    if (m.contains(QStringLiteral("time"))) q.asOf = m[QStringLiteral("time")].toInteger();
    if (m.contains(QStringLiteral("spark"))) {
      q.spark.clear();
      for (const auto& point : m[QStringLiteral("spark")].toArray()) q.spark.push_back(point.toDouble());
    }
    q.dirty = true;
  }
}

bool TerminalPage::isLive() const {
  return connected_ && feedUp_ && static_cast<double>(QDateTime::currentMSecsSinceEpoch()) - lastMessageMs_ < kStaleMs;
}

void TerminalPage::paintStatus() {
  const double now = static_cast<double>(QDateTime::currentMSecsSinceEpoch());
  const auto setBadge = [](QLabel* dot, QLabel* text, const QString& label, const QColor& colour) {
    text->setText(label);
    const QString style = QStringLiteral("color: %1;").arg(colourName(colour));
    if (text->styleSheet() != style) text->setStyleSheet(style);
    dot->setStyleSheet(QStringLiteral("background: %1; border-radius: 4px;").arg(colourName(colour)));
  };

  const bool live = isLive();
  if (live) {
    setBadge(cryptoDot_, cryptoStatus_, QStringLiteral("LIVE"), palette::kUp);
  } else if (!connected_) {
    setBadge(cryptoDot_, cryptoStatus_, QStringLiteral("CRYPTO OFF"), palette::kDown);
  } else if (retryAtMs_ > now) {
    setBadge(cryptoDot_, cryptoStatus_, QStringLiteral("RECONNECTING %1S").arg(std::ceil((retryAtMs_ - now) / 1000.0)), palette::kDown);
  } else if (hasFeedStatus_ && feedUp_) {
    setBadge(cryptoDot_, cryptoStatus_, QStringLiteral("NO DATA %1S").arg(std::floor((now - lastMessageMs_) / 1000.0)), palette::kWarn);
  } else {
    setBadge(cryptoDot_, cryptoStatus_, QStringLiteral("CONNECTING"), palette::kWarn);
  }

  if (!connected_ || !yahooUp_) {
    setBadge(yahooDot_, yahooStatus_, QStringLiteral("STOCKS OFF"), connected_ ? palette::kWarn : palette::kDown);
  } else if (yahooLimited_) {
    setBadge(yahooDot_, yahooStatus_, QStringLiteral("YAHOO PAUSED"), palette::kWarn);
  } else {
    setBadge(yahooDot_, yahooStatus_, QStringLiteral("YAHOO 90S"), palette::kWarn);
  }

  const QDateTime clock = QDateTime::currentDateTime();
  QString zone = clock.timeZoneAbbreviation();
  if (zone.size() > 5) {
    const int offset = clock.offsetFromUtc() / 3600;
    zone = offset == 0 ? QStringLiteral("UTC") : QStringLiteral("UTC%1%2").arg(offset > 0 ? QStringLiteral("+") : QString()).arg(offset);
  }
  clock_->setText(clock.toString(QStringLiteral("HH:mm:ss ")) + zone);
  if (meta_->text().isEmpty() || !connected_) meta_->setText(QStringLiteral("0 msg/s · -"));

  const bool stale = !live && everLive_;
  watchlist_->setDimmed(stale, connected_ && !yahooUp_);
  book_->setDimmed(stale);
}

void TerminalPage::paintQuote() {
  const bool market = isMarket(selected_);
  const Quote& q = quote(selected_);
  const auto change = changePercent(q.last, q.open);
  const auto [prefix, suffix] = market ? priceAffix(q.currency) : std::pair{format::currencySymbol(selected_.section(QLatin1Char('-'), 1)), QString()};
  bigPrice_->setText(q.last ? prefix + format::symbolPrice(market, selected_, q.last) + suffix : QStringLiteral("-"));
  const bool dim = market ? connected_ && !yahooUp_ : !isLive() && everLive_;
  const QString priceStyle = QStringLiteral("color: rgba(233,230,223,%1);").arg(dim ? 115 : 255);
  if (bigPrice_->styleSheet() != priceStyle) bigPrice_->setStyleSheet(priceStyle);

  bigChange_->setText(change ? format::percentArrow(change) + (market ? QStringLiteral(" TODAY") : QStringLiteral(" 24H")) : QStringLiteral("-"));
  const QString changeStyle = QStringLiteral("color: %1;").arg(colourName(signColour(change)));
  if (bigChange_->styleSheet() != changeStyle) bigChange_->setStyleSheet(changeStyle);

  QString note;
  if (market && q.asOf > 0) {
    const QString state = format::marketState(q.state);
    note = state + (q.delay && *q.delay > 0 ? QStringLiteral(", Yahoo delay %1m").arg(*q.delay) : QString()) + QStringLiteral(", as of ") + format::clockTime(q.asOf);
  }
  asOf_->setText(note);

  const QString title = market ? format::friendlyName(selected_) : selected_.section(QLatin1Char('-'), 0, 0);
  window()->setWindowTitle(q.last ? QStringLiteral("%1 %2 %3 Resolute Quant").arg(title, format::symbolPrice(market, selected_, q.last), change.value_or(0.0) >= 0.0 ? QStringLiteral("▲") : QStringLiteral("▼"))
                                  : QStringLiteral("Resolute Quant"));
}

void TerminalPage::paintLegend(int index) {
  const int count = chart_->candleCount();
  if (count == 0) {
    legend_->setText(QStringLiteral("&nbsp;"));
    return;
  }
  const int shown = index >= 0 && index < count ? index : count - 1;
  const Candle& c = chart_->candleAt(shown);
  const bool market = isMarket(selected_);
  const auto item = [](const QString& name, const QString& value) {
    return QStringLiteral("<span style=\"color:#8d9099\">%1</span> <span style=\"color:#e9e6df\">%2</span>&nbsp;&nbsp;&nbsp;").arg(name, value);
  };
  const auto price = [&](double value) { return format::symbolPrice(market, selected_, value); };
  legend_->setText(item(QStringLiteral("O"), price(c.open)) + item(QStringLiteral("H"), price(c.high)) + item(QStringLiteral("L"), price(c.low)) + item(QStringLiteral("C"), price(c.close)) +
                   item(QStringLiteral("VOL"), format::compact(c.hasVolume ? std::optional<double>(c.volume) : std::nullopt)) + QStringLiteral("<span style=\"color:#ffb000\">EMA 20</span>"));
}

void TerminalPage::paintCryptoStats() {
  const Quote& q = quote(selected_);
  const QString quoteCurrency = selected_.section(QLatin1Char('-'), 1);
  const QString base = selected_.section(QLatin1Char('-'), 0, 0);
  QVector<StatsStrip::Cell> cells(7);

  cells[0].label = QStringLiteral("24H RANGE");
  cells[0].valueSize = 14;
  cells[0].hasBar = true;
  cells[0].marker = 0.5;
  if (q.high && q.low) {
    cells[0].value = QStringLiteral("%1 to %2").arg(format::price(q.low), format::price(q.high));
    const double span = *q.high - *q.low;
    if (q.last) cells[0].marker = std::clamp((*q.last - *q.low) / (span != 0.0 ? span : 1.0), 0.0, 1.0);
  }

  cells[1].label = QStringLiteral("24H VOLUME");
  cells[1].note = QStringLiteral("-");
  if (q.volume && q.last) {
    cells[1].value = format::currencySymbol(quoteCurrency) + format::compact(*q.volume * *q.last);
    cells[1].note = QStringLiteral("%1 %2").arg(format::compact(q.volume), base);
  }

  cells[2].label = QStringLiteral("SESSION VWAP");
  cells[2].note = QStringLiteral("Waiting for the first trade");
  const auto vwap = numberIn(session_, "vwap");
  if (vwap && q.last) {
    const double difference = (*q.last / *vwap - 1.0) * 100.0;
    cells[2].value = format::price(vwap);
    cells[2].note = QStringLiteral("last %1%2% vs VWAP").arg(difference >= 0.0 ? QStringLiteral("+") : QString(), format::number(difference, 2));
  }

  cells[3].label = QStringLiteral("BUY / SELL FLOW");
  cells[3].hasBar = true;
  cells[3].fill = 0.5;
  if (const auto buy = numberIn(session_, "buyPercent")) {
    cells[3].value = QStringLiteral("%1% / %2%").arg(format::number(*buy, 0), format::number(100.0 - *buy, 0));
    cells[3].fill = *buy / 100.0;
  }

  cells[4].label = QStringLiteral("TRADES PER MIN");
  cells[4].note = QStringLiteral("-");
  if (!session_.isEmpty()) {
    cells[4].value = format::number(session_[QStringLiteral("tradesPerMinute")].toDouble(), 0);
    cells[4].note = QStringLiteral("%1 trades since %2").arg(format::number(session_[QStringLiteral("trades")].toDouble(), 0),
                                                             QDateTime::fromMSecsSinceEpoch(session_[QStringLiteral("since")].toInteger()).toString(QStringLiteral("HH:mm")));
  }

  cells[5].label = QStringLiteral("BOOK IMBALANCE");
  cells[5].note = QStringLiteral("bid vs ask, top 10 levels");
  if (const auto imbalance = book_->imbalance()) {
    cells[5].value = QStringLiteral("%1%2%").arg(*imbalance >= 0.0 ? QStringLiteral("+") : QString(), format::number(*imbalance, 1));
    cells[5].colour = signColour(imbalance);
  }

  cells[6].label = QStringLiteral("FEAR & GREED");
  cells[6].note = fearGreedNote_;
  if (fearGreedValue_) cells[6].value = QStringLiteral("%1 / 100").arg(*fearGreedValue_);

  stats_->setCells(cells);
  bookHead_->setNote(!bookReady_ ? (connected_ ? QStringLiteral("LOADING") : QStringLiteral("WAITING")) : !isLive() ? QStringLiteral("STALE") : QStringLiteral("LIVE, DEPTH %1").arg(format::number(bookDepth_, 0)));
}

void TerminalPage::paintMarketStats() {
  const Quote& q = quote(selected_);
  const auto pxs = [this](const std::optional<double>& value) { return format::symbolPrice(true, selected_, value); };
  const auto position = [&q](const std::optional<double>& low, const std::optional<double>& high) -> std::optional<double> {
    if (!low || !high || !q.last) return 0.5;
    const double span = *high - *low;
    return std::clamp((*q.last - *low) / (span != 0.0 ? span : 1.0), 0.0, 1.0);
  };
  const auto profileNumber = [this](const char* key) { return numberIn(profile_, key); };
  QVector<StatsStrip::Cell> cells(7);

  cells[0].label = QStringLiteral("DAY RANGE");
  cells[0].valueSize = 14;
  cells[0].hasBar = true;
  cells[0].marker = position(q.low, q.high);
  if (q.high) cells[0].value = QStringLiteral("%1 to %2").arg(pxs(q.low), pxs(q.high));

  const auto yearHigh = profileNumber("yearHigh") ? profileNumber("yearHigh") : q.yearHigh;
  const auto yearLow = profileNumber("yearLow") ? profileNumber("yearLow") : q.yearLow;
  cells[1].label = QStringLiteral("52 WEEK RANGE");
  cells[1].valueSize = 14;
  cells[1].hasBar = true;
  cells[1].marker = position(yearLow, yearHigh);
  if (yearHigh) cells[1].value = QStringLiteral("%1 to %2").arg(pxs(yearLow), pxs(yearHigh));

  cells[2].label = QStringLiteral("PREVIOUS CLOSE");
  cells[2].value = pxs(q.open);
  cells[2].note = q.open && q.last ? QStringLiteral("change %1%2").arg(*q.last >= *q.open ? QStringLiteral("+") : QStringLiteral("-"), pxs(std::abs(*q.last - *q.open))) : QStringLiteral("-");

  cells[3].label = QStringLiteral("VOLUME");
  if (q.volume && *q.volume != 0.0) cells[3].value = format::compact(q.volume);
  const auto average = profileNumber("avgVolume");
  cells[3].note = average ? QStringLiteral("average %1").arg(format::compact(average)) : QStringLiteral("not reported");

  cells[4].label = QStringLiteral("MARKET CAP");
  const auto cap = q.cap ? q.cap : profileNumber("cap");
  if (cap) cells[4].value = format::compact(cap);
  cells[4].note = q.currency.isEmpty() ? QStringLiteral("-") : QStringLiteral("quoted in %1").arg(q.currency);

  cells[5].label = QStringLiteral("P/E RATIO");
  const auto earnings = profileNumber("pe");
  if (earnings) cells[5].value = format::number(earnings, 1);
  cells[5].note = QStringLiteral("trailing 12 months");

  cells[6].label = QStringLiteral("MARKET STATE");
  const QString state = format::marketState(q.state);
  if (!state.isEmpty()) cells[6].value = state;
  cells[6].note = q.delay && *q.delay > 0 ? QStringLiteral("Yahoo lists a %1 min delay").arg(*q.delay) : QStringLiteral("no delay listed by Yahoo");

  stats_->setCells(cells);
}

void TerminalPage::paintWatchRow(const QString& symbol, Quote& q) {
  const bool market = isMarket(symbol);
  QString tag;
  if (market) {
    tag = format::marketState(q.state);
    tag.replace(QStringLiteral("AFTER HOURS"), QStringLiteral("AFTER")).replace(QStringLiteral("PRE-MARKET"), QStringLiteral("PRE"));
    if (tag.isEmpty()) tag = QStringLiteral("-");
    if (q.delay && *q.delay > 0) tag += QStringLiteral(" %1M").arg(*q.delay);
  }
  const auto change = changePercent(q.last, q.open);
  QVector<double> spark = q.spark;
  if (q.last) spark.push_back(*q.last);
  watchlist_->updateRow(symbol, format::symbolPrice(market, symbol, q.last), tag, format::percentArrow(change), signOf(change), spark, q.flash);
  q.flash = 0;
  q.dirty = false;
}

void TerminalPage::repaintAll() {
  paintStatus();
  for (const QString& symbol : markets_ + crypto_) {
    Quote& q = quote(symbol);
    if (q.dirty) paintWatchRow(symbol, q);
  }
  paintQuote();
  if (isMarket(selected_)) {
    paintMarketStats();
  } else {
    paintCryptoStats();
  }
  stats_->setDimmed(!isLive() && everLive_ && !isMarket(selected_));
}

void TerminalPage::toast(const QString& message, bool bad) {
  command_->clear();
  command_->setPlaceholderText(message);
  commandBox_->setStyleSheet(QStringLiteral("#commandBox { background: #07080a; border: 1px solid %1; }").arg(bad ? colourName(palette::kDown) : colourName(palette::kLine)));
  toastTimer_->start(kToastMs);
}

void TerminalPage::addSymbol(const QString& symbol, const QString& kind) {
  if (!connected_) {
    toast(QStringLiteral("Offline, so %1 can't be checked.").arg(symbol), true);
    return;
  }
  const quint64 id = engine_.submit(QStringLiteral("terminal_resolve"), QJsonObject{{QStringLiteral("symbol"), symbol}, {QStringLiteral("kind"), kind}});
  resolveJobs_.insert(id, QStringLiteral("Could not check %1. Check your connection.").arg(symbol));
}

void TerminalPage::addResolved(const QString& kind, const QString& symbol) {
  QStringList& list = kind == QLatin1String("market") ? markets_ : crypto_;
  if (list.contains(symbol)) {
    select(symbol);
    return;
  }
  if (list.size() >= kMaxWatch) {
    toast(QStringLiteral("The %1 watchlist is full at %2 symbols. Remove one with DEL first.").arg(kind == QLatin1String("market") ? QStringLiteral("markets") : QStringLiteral("crypto")).arg(kMaxWatch), true);
    return;
  }
  list << symbol;
  saveSettings();
  watchlist_->setSymbols(markets_, crypto_);
  pushWatch();
  select(symbol);
}

void TerminalPage::removeSymbol(const QString& symbol) {
  const bool market = isMarket(symbol);
  if (!market && !crypto_.contains(symbol)) {
    toast(QStringLiteral("%1 is not on your watchlist.").arg(symbol), true);
    return;
  }
  if (markets_.size() + crypto_.size() == 1 || (!market && crypto_.size() == 1)) {
    toast(QStringLiteral("Keep at least one symbol on the list."), true);
    return;
  }
  if (symbol == selected_) {
    const QStringList all = crypto_ + markets_;
    select(*std::find_if(all.cbegin(), all.cend(), [&](const QString& s) { return s != symbol; }));
  }
  (market ? markets_ : crypto_).removeAll(symbol);
  saveSettings();
  watchlist_->setSymbols(markets_, crypto_);
  pushWatch();
}

void TerminalPage::openSymbol(const QString& word) {
  const QString plain = QString(word).remove(QRegularExpression(QStringLiteral("\\W")));
  for (const QString& symbol : markets_) {
    if (format::friendlyName(symbol) != symbol && QString(format::friendlyName(symbol)).remove(QRegularExpression(QStringLiteral("\\W"))) == plain) {
      select(symbol);
      return;
    }
  }
  if (isMarket(word)) {
    select(word);
  } else if (crypto_.contains(normaliseCrypto(word))) {
    select(normaliseCrypto(word));
  } else {
    addSymbol(word, QStringLiteral("auto"));
  }
}

void TerminalPage::runCommand(const QString& text) {
  const QStringList words = text.trimmed().toUpper().split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
  if (words.isEmpty()) return;
  const QString a = words[0];
  const QString b = words.value(1);
  for (const auto& frame : timeframes()) {
    if (frame.first == a) {
      setTimeframe(a);
      return;
    }
  }
  if (a == QLatin1String("HELP")) {
    showHelp();
  } else if (a == QLatin1String("HISTORY")) {
    emit historyRequested(selected_, isMarket(selected_));
  } else if (a == QLatin1String("HEALTH")) {
    emit healthRequested();
  } else if (a == QLatin1String("STK") && !b.isEmpty()) {
    if (isMarket(b)) select(b);
    else addSymbol(b, QStringLiteral("market"));
  } else if (a == QLatin1String("ADD") && !b.isEmpty()) {
    openSymbol(b);
  } else if ((a == QLatin1String("DEL") || a == QLatin1String("REMOVE")) && !b.isEmpty()) {
    removeSymbol(isMarket(b) ? b : normaliseCrypto(b));
  } else if (a == QLatin1String("CLEAR")) {
    alerts_.erase(std::remove_if(alerts_.begin(), alerts_.end(), [this](const Alert& alert) { return alert.symbol == selected_; }), alerts_.end());
    saveSettings();
    refreshAlertLines();
    toast(QStringLiteral("Alerts cleared on %1.").arg(selected_));
  } else if (a == QLatin1String("ALERT")) {
    bool ok = false;
    const double price = QString(b).remove(QLatin1Char(',')).toDouble(&ok);
    if (!ok || price <= 0.0) {
      toast(QStringLiteral("Give a price, for example ALERT 77000."), true);
      return;
    }
    alerts_.push_back({selected_, price});
    saveSettings();
    refreshAlertLines();
    toast(QStringLiteral("Alert set on %1 at %2.").arg(selected_, format::price(price)));
  } else {
    openSymbol(a);
  }
}

void TerminalPage::checkAlerts(const QString& symbol, std::optional<double> before, double after) {
  if (!before || alerts_.isEmpty()) return;
  bool fired = false;
  for (int i = static_cast<int>(alerts_.size()) - 1; i >= 0; --i) {
    const Alert alert = alerts_[i];
    if (alert.symbol == symbol && ((*before < alert.price && after >= alert.price) || (*before > alert.price && after <= alert.price))) {
      showAlert(QStringLiteral("%1 crossed %2 at %3. Last %4.").arg(symbol, format::price(alert.price), format::clockTime(QDateTime::currentMSecsSinceEpoch()), format::price(after)));
      alerts_.removeAt(i);
      fired = true;
    }
  }
  if (fired) {
    saveSettings();
    refreshAlertLines();
  }
}

void TerminalPage::showAlert(const QString& text) {
  alertText_->setText(text);
  alertBar_->show();
}

void TerminalPage::refreshAlertLines() {
  QVector<double> levels;
  for (const Alert& alert : alerts_) {
    if (alert.symbol == selected_) levels.push_back(alert.price);
  }
  chart_->setAlertLevels(levels);
}

}  // namespace rq
