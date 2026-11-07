#include "app/TerminalWidgets.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <algorithm>
#include <cmath>

#include "app/Format.h"
#include "app/Palette.h"

namespace rq {

namespace {

constexpr int kRowHeight = 35;
constexpr int kGroupHeight = 23;
constexpr int kPadding = 10;
constexpr int kGap = 6;
constexpr int kSymbolWidth = 66;
constexpr int kSparkWidth = 44;
constexpr int kSparkHeight = 22;
constexpr int kChangeWidth = 58;
constexpr int kFlashMs = 500;
constexpr int kBookLevels = 10;
constexpr int kLevelHeight = 18;
constexpr int kSpreadHeight = 24;
constexpr int kTapeHeaderHeight = 20;
constexpr int kTradeHeight = 17;
constexpr double kBigTradeValue = 50000.0;  // The same threshold as the browser terminal.
constexpr double kDimmedOpacity = 0.45;

}  // namespace

PanelHead::PanelHead(QString title, QString note, QWidget* parent) : QWidget(parent), title_(std::move(title)), note_(std::move(note)) {
  setFixedHeight(26);
}

void PanelHead::setNote(const QString& note) {
  if (note == note_) return;
  note_ = note;
  update();
}

void PanelHead::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.fillRect(rect(), palette::kPanel);
  painter.setPen(palette::kLine);
  painter.drawLine(0, height() - 1, width(), height() - 1);
  painter.setFont(palette::label(12, 600, 0.1));
  painter.setPen(palette::kAmber);
  painter.drawText(rect().adjusted(kPadding, 0, 0, -1), Qt::AlignLeft | Qt::AlignVCenter, title_);
  painter.setFont(palette::mono(11));
  painter.setPen(palette::kMute);
  painter.drawText(rect().adjusted(0, 0, -kPadding, -1), Qt::AlignRight | Qt::AlignVCenter, note_);
}

WatchlistView::WatchlistView(QWidget* parent) : QWidget(parent), flashTimer_(new QTimer(this)) {
  setObjectName(QStringLiteral("watchlist"));
  setMouseTracking(true);
  connect(flashTimer_, &QTimer::timeout, this, qOverload<>(&QWidget::update));
  flashTimer_->setInterval(33);
}

void WatchlistView::setSymbols(const QStringList& markets, const QStringList& crypto) {
  QVector<Row> next;
  const auto take = [&](const QString& symbol, bool market) {
    const auto found = std::find_if(rows_.begin(), rows_.end(), [&](const Row& r) { return r.symbol == symbol; });
    if (found != rows_.end()) {
      next.push_back(*found);
      return;
    }
    Row row;
    row.symbol = symbol;
    row.market = market;
    row.title = market ? format::friendlyName(symbol) : symbol.section(QLatin1Char('-'), 0, 0);
    row.tag = market ? QStringLiteral("-") : symbol.section(QLatin1Char('-'), 1);
    next.push_back(row);
  };
  for (const QString& symbol : markets) take(symbol, true);
  for (const QString& symbol : crypto) take(symbol, false);
  rows_ = std::move(next);
  marketCount_ = static_cast<int>(markets.size());
  updateGeometry();
  setMinimumHeight(sizeHint().height());
  update();
}

void WatchlistView::updateRow(const QString& symbol, const QString& price, const QString& tag, const QString& change, int changeSign, const QVector<double>& spark, int flash) {
  for (Row& row : rows_) {
    if (row.symbol != symbol) continue;
    row.price = price;
    if (!tag.isEmpty()) row.tag = tag;
    row.change = change;
    row.changeSign = changeSign;
    row.spark = spark;
    if (flash != 0) {
      row.flash = flash;
      row.flashClock.start();
      flashTimer_->start();
    }
    update();
    return;
  }
}

void WatchlistView::setSelected(const QString& symbol) {
  selected_ = symbol;
  update();
}

void WatchlistView::setNote(const QString& note) {
  if (note == note_) return;
  note_ = note;
  setMinimumHeight(sizeHint().height());
  updateGeometry();
  update();
}

void WatchlistView::setDimmed(bool crypto, bool markets) {
  if (crypto == dimCrypto_ && markets == dimMarkets_) return;
  dimCrypto_ = crypto;
  dimMarkets_ = markets;
  update();
}

const WatchlistView::Row* WatchlistView::row(const QString& symbol) const {
  const auto found = std::find_if(rows_.cbegin(), rows_.cend(), [&](const Row& r) { return r.symbol == symbol; });
  return found == rows_.cend() ? nullptr : &*found;
}

QPoint WatchlistView::rowCentre(const QString& symbol) const {
  for (int i = 0; i < rows_.size(); ++i) {
    if (rows_[i].symbol == symbol) return QPoint(width() / 2, yOfRow(i) + kRowHeight / 2);
  }
  return {};
}

int WatchlistView::noteHeight() const { return note_.isEmpty() ? 0 : 54; }

QSize WatchlistView::sizeHint() const { return QSize(292, kGroupHeight * 2 + noteHeight() + static_cast<int>(rows_.size()) * kRowHeight); }

int WatchlistView::yOfRow(int index) const {
  if (index < marketCount_) return kGroupHeight + noteHeight() + index * kRowHeight;
  return kGroupHeight * 2 + noteHeight() + index * kRowHeight;
}

int WatchlistView::rowAt(int y) const {
  for (int i = 0; i < rows_.size(); ++i) {
    const int top = yOfRow(i);
    if (y >= top && y < top + kRowHeight) return i;
  }
  return -1;
}

void WatchlistView::paintGroup(QPainter& painter, int y, const QString& left, const QString& right) {
  const QRect area(0, y, width(), kGroupHeight);
  painter.fillRect(area, palette::kPanel);
  painter.setPen(palette::kLine);
  painter.drawLine(0, area.bottom(), width(), area.bottom());
  painter.setPen(palette::kMute);
  painter.setFont(palette::label(11, 600, 0.08));
  painter.drawText(area.adjusted(kPadding, 0, 0, -1), Qt::AlignLeft | Qt::AlignVCenter, left);
  painter.setFont(palette::label(11, 500, 0.04));
  painter.drawText(area.adjusted(0, 0, -kPadding, -1), Qt::AlignRight | Qt::AlignVCenter, right);
}

void WatchlistView::paintRow(QPainter& painter, int y, Row& row) {
  const QRect area(0, y, width(), kRowHeight);
  const bool selected = row.symbol == selected_;
  const int index = static_cast<int>(&row - rows_.data());
  if (selected || index == hovered_) painter.fillRect(area, palette::kRaise);
  if (selected) painter.fillRect(0, y, 2, kRowHeight, palette::kAmber);
  painter.setPen(palette::kRowLine);
  painter.drawLine(0, area.bottom(), width(), area.bottom());

  // Grid columns: 66px, 44px, the rest, 58px, with 6px gaps inside 10px padding.
  const int symbolLeft = kPadding;
  const int sparkLeft = symbolLeft + kSymbolWidth + kGap;
  const int priceLeft = sparkLeft + kSparkWidth + kGap;
  const int changeLeft = width() - kPadding - kChangeWidth;
  const int priceWidth = changeLeft - kGap - priceLeft;

  QFont title = palette::mono(13, 600);
  QFontMetrics titleMetrics(title);
  painter.setFont(title);
  painter.setPen(palette::kForeground);
  painter.drawText(QRect(symbolLeft, y + 3, kSymbolWidth, 16), Qt::AlignLeft | Qt::AlignVCenter, titleMetrics.elidedText(row.title, Qt::ElideNone, kSymbolWidth));
  painter.setFont(palette::mono(10));
  painter.setPen(palette::kMute);
  painter.drawText(QRect(symbolLeft, y + 19, kSymbolWidth, 12), Qt::AlignLeft | Qt::AlignVCenter, row.tag);

  if (row.spark.size() >= 2) {
    const auto [low, high] = std::minmax_element(row.spark.cbegin(), row.spark.cend());
    const double range = *high - *low != 0.0 ? *high - *low : 1.0;
    const int n = static_cast<int>(row.spark.size()) - 1;
    const double scaleX = static_cast<double>(kSparkWidth) / 52.0;
    const double scaleY = static_cast<double>(kSparkHeight) / 24.0;
    const int top = y + (kRowHeight - kSparkHeight) / 2;
    QPainterPath path;
    for (int i = 0; i <= n; ++i) {
      const QPointF point(sparkLeft + static_cast<double>(i) / n * 52.0 * scaleX, top + (22.0 - (row.spark[i] - *low) / range * 20.0) * scaleY);
      if (i == 0) path.moveTo(point);
      else path.lineTo(point);
    }
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.strokePath(path, QPen(row.spark.last() >= row.spark.first() ? palette::kUp : palette::kDown, 1.0));
    painter.restore();
  }

  const QRect priceArea(priceLeft, y, priceWidth, kRowHeight);
  if (row.flash != 0) {
    const double progress = row.flashClock.elapsed() / static_cast<double>(kFlashMs);
    if (progress >= 1.0) {
      row.flash = 0;
    } else {
      QColor flash = row.flash > 0 ? palette::kUpBackground : palette::kDownBackground;
      flash.setAlphaF(flash.alphaF() * (1.0 - progress));
      painter.fillRect(priceArea.adjusted(0, 8, 0, -8), flash);
    }
  }
  const bool dim = row.market ? dimMarkets_ : dimCrypto_;
  painter.save();
  painter.setOpacity(dim ? kDimmedOpacity : 1.0);
  painter.setFont(palette::mono(13));
  painter.setPen(palette::kForeground);
  painter.drawText(priceArea, Qt::AlignRight | Qt::AlignVCenter, row.price);
  painter.restore();

  QFont changeFont = palette::mono(11);
  changeFont.setPixelSize(12);
  painter.setFont(changeFont);
  painter.setPen(row.changeSign > 0 ? palette::kUp : row.changeSign < 0 ? palette::kDown : palette::kMute);
  painter.drawText(QRect(changeLeft, y, kChangeWidth, kRowHeight), Qt::AlignRight | Qt::AlignVCenter, row.change);
}

void WatchlistView::paintEvent(QPaintEvent* event) {
  QPainter painter(this);
  painter.fillRect(event->rect(), palette::kBackground);
  paintGroup(painter, 0, QStringLiteral("MARKETS"), QStringLiteral("YAHOO, CHANGE TODAY"));
  if (!note_.isEmpty()) {
    const QRect area(0, kGroupHeight, width(), noteHeight());
    painter.setFont(palette::mono(12));
    painter.setPen(palette::kWarn);
    painter.drawText(area.adjusted(kPadding, 0, -kPadding, -1), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, note_);
    painter.setPen(palette::kLine);
    painter.drawLine(0, area.bottom(), width(), area.bottom());
  }
  paintGroup(painter, kGroupHeight + noteHeight() + marketCount_ * kRowHeight, QStringLiteral("CRYPTO"), QStringLiteral("COINBASE, 24H CHANGE"));
  bool anyFlash = false;
  for (int i = 0; i < rows_.size(); ++i) {
    const int top = yOfRow(i);
    if (top + kRowHeight >= event->rect().top() && top <= event->rect().bottom()) paintRow(painter, top, rows_[i]);
    anyFlash = anyFlash || rows_[i].flash != 0;
  }
  if (!anyFlash) flashTimer_->stop();
}

void WatchlistView::mouseMoveEvent(QMouseEvent* event) {
  const int index = rowAt(static_cast<int>(event->position().y()));
  setCursor(index >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
  if (index == hovered_) return;
  hovered_ = index;
  update();
}

void WatchlistView::mouseReleaseEvent(QMouseEvent* event) {
  const int index = rowAt(static_cast<int>(event->position().y()));
  if (event->button() == Qt::LeftButton && index >= 0) emit symbolClicked(rows_[index].symbol);
}

void WatchlistView::leaveEvent(QEvent*) {
  hovered_ = -1;
  update();
}

OrderBookView::OrderBookView(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("orderBook"));
  setFixedHeight(sizeHint().height());
}

QSize OrderBookView::sizeHint() const { return QSize(296, (kLevelHeight * kBookLevels + 4) * 2 + kSpreadHeight); }

void OrderBookView::clearBook() {
  bids_.clear();
  asks_.clear();
  spread_ = mid_ = QStringLiteral("-");
  imbalance_.reset();
  update();
}

void OrderBookView::setBook(const QJsonArray& bids, const QJsonArray& asks) {
  const auto build = [](const QJsonArray& side) {
    QVector<Level> levels;
    double cumulative = 0.0;
    for (int i = 0; i < std::min<qsizetype>(side.size(), kBookLevels); ++i) {
      const QJsonArray level = side[i].toArray();
      cumulative += level[1].toDouble();
      levels.push_back({level[0].toDouble(), level[1].toDouble(), cumulative});
    }
    return levels;
  };
  bids_ = build(bids);
  asks_ = build(asks);
  if (!asks_.isEmpty() && !bids_.isEmpty()) {
    const double ask = asks_[0].price;
    const double bid = bids_[0].price;
    const double middle = (ask + bid) / 2.0;
    spread_ = QStringLiteral("%1 (%2 bp)").arg(format::number(ask - bid, format::decimalsFor(middle)), format::number((ask - bid) / middle * 1e4, 2));
    mid_ = format::price(middle);
  }
  const double totalBid = bids_.isEmpty() ? 0.0 : bids_.last().cumulative;
  const double totalAsk = asks_.isEmpty() ? 0.0 : asks_.last().cumulative;
  const double total = totalBid + totalAsk;
  imbalance_ = total > 0.0 ? std::optional<double>((totalBid - totalAsk) / total * 100.0) : std::nullopt;
  update();
}

void OrderBookView::setDimmed(bool dimmed) {
  if (dimmed == dimmed_) return;
  dimmed_ = dimmed;
  update();
}

void OrderBookView::paintLevels(QPainter& painter, int top, const QVector<Level>& levels, bool asks, double maximum) {
  const int third = (width() - kPadding * 2) / 3;
  for (int slot = 0; slot < kBookLevels; ++slot) {
    // Asks read upwards from the spread, so the best ask sits at the bottom.
    const int index = asks ? kBookLevels - 1 - slot : slot;
    if (index >= levels.size()) continue;
    const Level& level = levels[index];
    const QRect area(0, top + 2 + slot * kLevelHeight, width(), kLevelHeight);
    const int barWidth = static_cast<int>(level.cumulative / maximum * width());
    painter.fillRect(width() - barWidth, area.top() + 1, barWidth, kLevelHeight - 2, asks ? palette::kDownBackground : palette::kUpBackground);
    painter.save();
    painter.setOpacity(dimmed_ ? kDimmedOpacity : 1.0);
    painter.setPen(asks ? palette::kDown : palette::kUp);
    painter.drawText(QRect(kPadding, area.top(), third, kLevelHeight), Qt::AlignLeft | Qt::AlignVCenter, format::price(level.price));
    painter.setPen(palette::kForeground);
    painter.drawText(QRect(kPadding + third, area.top(), third, kLevelHeight), Qt::AlignRight | Qt::AlignVCenter, format::size(level.size));
    painter.drawText(QRect(kPadding + third * 2, area.top(), width() - kPadding * 2 - third * 2, kLevelHeight), Qt::AlignRight | Qt::AlignVCenter, format::size(level.cumulative));
    painter.restore();
  }
}

void OrderBookView::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.fillRect(rect(), palette::kBackground);
  painter.setFont(palette::mono(12));
  const double maximum = std::max({asks_.isEmpty() ? 1.0 : asks_.last().cumulative, bids_.isEmpty() ? 1.0 : bids_.last().cumulative, 1e-12});
  const int blockHeight = kLevelHeight * kBookLevels + 4;
  paintLevels(painter, 0, asks_, true, maximum);

  const QRect spread(0, blockHeight, width(), kSpreadHeight);
  painter.fillRect(spread, palette::kPanel);
  painter.setPen(palette::kLine);
  painter.drawLine(0, spread.top(), width(), spread.top());
  painter.drawLine(0, spread.bottom(), width(), spread.bottom());
  const QFont plain = palette::mono(12);
  const QFont bold = palette::mono(12, 500);
  const QFontMetrics plainMetrics(plain);
  const QFontMetrics boldMetrics(bold);
  painter.setFont(plain);
  painter.setPen(palette::kMute);
  const QString spreadLabel = QStringLiteral("SPREAD ");
  painter.drawText(spread.adjusted(kPadding, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, spreadLabel);
  const int midWidth = boldMetrics.horizontalAdvance(mid_);
  painter.drawText(spread.adjusted(0, 0, -kPadding - midWidth, 0), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("MID "));
  painter.setFont(bold);
  painter.setPen(palette::kForeground);
  painter.drawText(spread.adjusted(kPadding + plainMetrics.horizontalAdvance(spreadLabel), 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, spread_);
  painter.drawText(spread.adjusted(0, 0, -kPadding, 0), Qt::AlignRight | Qt::AlignVCenter, mid_);

  painter.setFont(plain);
  paintLevels(painter, blockHeight + kSpreadHeight, bids_, false, maximum);
}

TapeView::TapeView(QWidget* parent) : QWidget(parent) { setObjectName(QStringLiteral("tape")); }

void TapeView::setTrades(const QJsonArray& trades) {
  trades_ = trades;
  update();
}

void TapeView::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.fillRect(rect(), palette::kBackground);
  const int available = width() - kPadding * 2;
  const int timeWidth = static_cast<int>(available * 1.1 / 2.95);
  const int priceWidth = static_cast<int>(available * 1.0 / 2.95);
  const int sizeWidth = available - timeWidth - priceWidth;

  painter.setFont(palette::mono(11));
  painter.setPen(palette::kMute);
  painter.drawText(QRect(kPadding, 0, timeWidth, kTapeHeaderHeight), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("TIME"));
  painter.drawText(QRect(kPadding + timeWidth, 0, priceWidth, kTapeHeaderHeight), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("PRICE"));
  painter.drawText(QRect(kPadding + timeWidth + priceWidth, 0, sizeWidth, kTapeHeaderHeight), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("SIZE"));

  int y = kTapeHeaderHeight;
  for (const auto& entry : trades_) {
    if (y >= height()) break;
    const QJsonObject trade = entry.toObject();
    const double price = trade[QStringLiteral("p")].toDouble();
    const double size = trade[QStringLiteral("s")].toDouble();
    const bool big = price * size >= kBigTradeValue;
    if (big) painter.fillRect(0, y, width(), kTradeHeight, palette::kRaise);
    painter.setFont(palette::mono(12, big ? 600 : 400));
    painter.setPen(palette::kMute);
    painter.drawText(QRect(kPadding, y, timeWidth, kTradeHeight), Qt::AlignLeft | Qt::AlignVCenter, format::clockTime(trade[QStringLiteral("t")].toInteger()));
    painter.setPen(trade[QStringLiteral("side")].toString() == QStringLiteral("buy") ? palette::kUp : palette::kDown);
    painter.drawText(QRect(kPadding + timeWidth, y, priceWidth, kTradeHeight), Qt::AlignRight | Qt::AlignVCenter, format::price(price));
    painter.setPen(palette::kForeground);
    painter.drawText(QRect(kPadding + timeWidth + priceWidth, y, sizeWidth, kTradeHeight), Qt::AlignRight | Qt::AlignVCenter, format::size(size));
    y += kTradeHeight;
  }
}

StatsStrip::StatsStrip(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("stats"));
  setFixedHeight(84);
}

void StatsStrip::setCells(const QVector<Cell>& cells) {
  cells_ = cells;
  update();
}

void StatsStrip::setDimmed(bool dimmed) {
  if (dimmed == dimmed_) return;
  dimmed_ = dimmed;
  update();
}

void StatsStrip::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.fillRect(rect(), palette::kLine);
  if (cells_.isEmpty()) return;
  const int count = static_cast<int>(cells_.size());
  const double cellWidth = (width() - (count - 1)) / static_cast<double>(count);
  for (int i = 0; i < count; ++i) {
    const StatsStrip::Cell& cell = cells_[i];
    const QRect area(static_cast<int>(i * (cellWidth + 1)), 0, static_cast<int>(cellWidth + 0.5), height());
    painter.fillRect(area, palette::kBackground);
    const QRect inner = area.adjusted(11, 8, -11, -2);
    painter.setFont(palette::label(11, 600, 0.08));
    painter.setPen(palette::kAmber);
    painter.drawText(QRect(inner.left(), inner.top(), inner.width(), 15), Qt::AlignLeft | Qt::AlignVCenter, cell.label);

    painter.save();
    painter.setOpacity(dimmed_ ? kDimmedOpacity : 1.0);
    painter.setFont(palette::mono(cell.valueSize, 500));
    painter.setPen(cell.colour.isValid() ? cell.colour : palette::kForeground);
    painter.drawText(QRect(inner.left(), inner.top() + 17, inner.width(), 23), Qt::AlignLeft | Qt::AlignVCenter, cell.value);
    painter.restore();

    if (cell.hasBar) {
      const QRect bar(inner.left(), inner.top() + 45, inner.width(), 6);
      painter.fillRect(bar, palette::kRaise);
      if (cell.fill) painter.fillRect(bar.left(), bar.top(), static_cast<int>(*cell.fill * bar.width()), bar.height(), palette::kUp);
      if (cell.marker) painter.fillRect(bar.left() + static_cast<int>(*cell.marker * (bar.width() - 2)), bar.top() - 1, 2, bar.height() + 2, palette::kAmber);
    } else {
      const QFont font = palette::mono(11);
      painter.setFont(font);
      painter.setPen(palette::kMute);
      const QFontMetrics metrics(font);
      painter.drawText(QRect(inner.left(), inner.top() + 41, inner.width(), 16), Qt::AlignLeft | Qt::AlignVCenter, metrics.elidedText(cell.note, Qt::ElideRight, inner.width()));
    }
  }
}

}  // namespace rq
