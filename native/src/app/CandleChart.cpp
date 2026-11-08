#include "app/CandleChart.h"

#include <QDateTime>
#include <QKeyEvent>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimeZone>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <limits>

#include "app/Palette.h"

namespace rq {

namespace {

constexpr int kMinVisible = 10;
constexpr int kAxisWidth = 84;
constexpr int kTimeAxisHeight = 24;
constexpr int kReadoutHeight = 26;
constexpr double kVolumeShare = 0.18;
constexpr int kMinLabelSpacing = 110;
constexpr int kEmaPeriod = 20;
constexpr int kTerminalBarSpacing = 8;
constexpr int kTerminalRightOffset = 6;
// The browser chart's price scale margins.
constexpr double kPriceTopMargin = 0.06;
constexpr double kPriceBottomMargin = 0.22;
constexpr double kVolumeTop = 0.84;

QString exact(double value) { return QString::number(value, 'f', QLocale::FloatingPointShortest); }

QString utcText(qint64 seconds, bool intraday) {
  const QDateTime when = QDateTime::fromSecsSinceEpoch(seconds, QTimeZone::UTC);
  return when.toString(intraday ? QStringLiteral("yyyy-MM-dd HH:mm") : QStringLiteral("yyyy-MM-dd"));
}

// A round spacing that gives about the requested number of grid lines.
double niceStep(double range, int lines) {
  const double rough = range / std::max(lines, 1);
  const double magnitude = std::pow(10.0, std::floor(std::log10(rough)));
  const double scaled = rough / magnitude;
  const double nice = scaled < 1.5 ? 1.0 : scaled < 3.0 ? 2.0 : scaled < 7.0 ? 5.0 : 10.0;
  return nice * magnitude;
}

}  // namespace

CandleChart::CandleChart(QWidget* parent) : QWidget(parent) {
  setMouseTracking(true);
  setFocusPolicy(Qt::StrongFocus);
  setMinimumHeight(220);
  setAccessibleName(QStringLiteral("Price chart"));
}

void CandleChart::setTerminalStyle(bool terminal) {
  terminal_ = terminal;
  update();
}

void CandleChart::rebuildEma() {
  ema_.resize(candles_.size());
  const double k = 2.0 / (kEmaPeriod + 1);
  for (int i = 0; i < candles_.size(); ++i) {
    ema_[i] = i == 0 ? candles_[0].close : candles_[i].close * k + ema_[i - 1] * (1.0 - k);
  }
}

void CandleChart::setCandles(QVector<Candle> candles, bool intraday, bool keepView) {
  const bool wasFollowing = first_ + count_ == candleCount();
  const int previousCount = count_;
  const int previousFirst = first_;
  candles_ = std::move(candles);
  intraday_ = intraday;
  anyVolume_ = std::any_of(candles_.cbegin(), candles_.cend(), [](const Candle& c) { return c.hasVolume; });
  emptyMessage_.clear();
  rebuildEma();
  setHovered(-1);
  if (keepView && previousCount > 0 && userMovedView_) {
    count_ = previousCount;
    first_ = wasFollowing ? candleCount() - count_ : previousFirst;
    clampView();
  } else {
    userMovedView_ = false;
    applyDefaultView();
  }
  update();
}

void CandleChart::clear(const QString& emptyMessage) {
  candles_.clear();
  ema_.clear();
  anyVolume_ = false;
  emptyMessage_ = emptyMessage;
  first_ = count_ = 0;
  userMovedView_ = false;
  setHovered(-1);
  update();
}

void CandleChart::updateLatest(const Candle& candle) {
  if (candles_.isEmpty() || candle.time < candles_.last().time) return;
  anyVolume_ = anyVolume_ || candle.hasVolume;
  const double k = 2.0 / (kEmaPeriod + 1);
  if (candle.time == candles_.last().time) {
    candles_.last() = candle;
    const int last = candleCount() - 1;
    ema_[last] = last == 0 ? candle.close : candle.close * k + ema_[last - 1] * (1.0 - k);
  } else {
    const bool followNewest = first_ + count_ == candleCount();
    const bool showingAll = followNewest && first_ == 0;
    ema_.push_back(candle.close * k + ema_.last() * (1.0 - k));
    candles_.push_back(candle);
    if (showingAll) {
      ++count_;
    } else if (followNewest) {
      ++first_;
    }
    if (terminal_ && !userMovedView_) applyDefaultView();
  }
  update();
}

void CandleChart::setPriceDecimals(int decimals) {
  priceDecimals_ = decimals;
  update();
}

void CandleChart::setAlertLevels(QVector<double> levels) {
  alerts_ = std::move(levels);
  update();
}

void CandleChart::setMessage(const QString& message) {
  message_ = message;
  update();
}

int CandleChart::rightOffset() const { return terminal_ ? kTerminalRightOffset : 0; }

QRect CandleChart::plotRect() const {
  const int top = terminal_ ? 0 : kReadoutHeight;
  return QRect(0, top, std::max(width() - kAxisWidth, 1), std::max(height() - top - kTimeAxisHeight, 1));
}

void CandleChart::clampView() {
  const int total = candleCount();
  count_ = std::clamp(count_, std::min(kMinVisible, total), total);
  first_ = std::clamp(first_, 0, total - count_);
}

void CandleChart::applyDefaultView() {
  if (terminal_) {
    const int fit = plotRect().width() / kTerminalBarSpacing - kTerminalRightOffset;
    count_ = std::clamp(fit, std::min(kMinVisible, candleCount()), candleCount());
    first_ = candleCount() - count_;
  } else {
    first_ = 0;
    count_ = candleCount();
  }
}

void CandleChart::showAll() {
  userMovedView_ = false;
  first_ = 0;
  count_ = candleCount();
  update();
}

void CandleChart::resizeEvent(QResizeEvent*) {
  if (terminal_ && !userMovedView_ && !candles_.isEmpty()) applyDefaultView();
}

void CandleChart::zoomBy(double factor, double anchorFraction) {
  if (candles_.isEmpty()) return;
  userMovedView_ = true;
  const double anchor = first_ + std::clamp(anchorFraction, 0.0, 1.0) * count_;
  const int newCount = std::clamp(static_cast<int>(std::lround(count_ / factor)), std::min(kMinVisible, candleCount()), candleCount());
  first_ = static_cast<int>(std::lround(anchor - std::clamp(anchorFraction, 0.0, 1.0) * newCount));
  count_ = newCount;
  clampView();
  update();
}

void CandleChart::panBy(int candles) {
  userMovedView_ = true;
  first_ += candles;
  clampView();
  update();
}

int CandleChart::indexAtX(int x) const {
  const QRect plot = plotRect();
  if (count_ <= 0 || x < plot.left() || x > plot.right()) return -1;
  const int positions = count_ + rightOffset();
  const int index = first_ + static_cast<int>((x - plot.left()) * static_cast<double>(positions) / plot.width());
  return index < first_ + count_ ? index : -1;
}

QString CandleChart::readout(int index) const {
  if (index < 0 || index >= candleCount()) return {};
  const Candle& c = candles_[index];
  return QStringLiteral("%1 UTC  O %2  H %3  L %4  C %5  V %6")
      .arg(utcText(c.time, intraday_), exact(c.open), exact(c.high), exact(c.low), exact(c.close),
           c.hasVolume ? exact(c.volume) : QStringLiteral("not reported"));
}

QString CandleChart::priceText(double price, double step) const {
  if (priceDecimals_ >= 0) return QString::number(price, 'f', priceDecimals_);
  const int decimals = std::clamp(static_cast<int>(-std::floor(std::log10(step))), 0, 8);
  return QString::number(price, 'f', decimals);
}

void CandleChart::paintTimeLabels(QPainter& painter, const QRect& plot, double barWidth) {
  const auto xFor = [&](int index) { return static_cast<int>(std::lround(plot.left() + (index - first_ + 0.5) * barWidth)); };
  QRect axis(plot.left(), plot.bottom() + 2, plot.width(), kTimeAxisHeight - 2);

  if (!terminal_) {
    const int every = std::max(1, static_cast<int>(std::ceil(kMinLabelSpacing / barWidth)));
    for (int i = first_ + every / 2; i < first_ + count_; i += every) {
      const int x = xFor(i);
      painter.setPen(palette::kLine);
      painter.drawLine(x, plot.top(), x, plot.bottom());
      painter.setPen(palette::kMute);
      painter.drawText(QRect(x - kMinLabelSpacing / 2, axis.top(), kMinLabelSpacing, axis.height()), Qt::AlignCenter, utcText(candles_[i].time, intraday_));
    }
    return;
  }

  // Labels fall on round times, as in the browser chart, with hours and days in bold.
  const int spacing = 80;
  const int every = std::max(1, static_cast<int>(std::ceil(spacing / barWidth)));
  qint64 barSeconds = candleCount() > 1 ? candles_[candleCount() - 1].time - candles_[candleCount() - 2].time : 60;
  for (int i = candleCount() - 1; i > 0 && i > candleCount() - 20; --i) barSeconds = std::min(barSeconds, candles_[i].time - candles_[i - 1].time);
  barSeconds = std::max<qint64>(barSeconds, 60);
  qint64 labelSeconds = barSeconds * every;
  for (const qint64 nice : {300LL, 600LL, 900LL, 1800LL, 3600LL, 7200LL, 14400LL, 21600LL, 43200LL, 86400LL, 172800LL, 604800LL, 1209600LL, 2592000LL, 7776000LL, 31536000LL}) {
    if (labelSeconds <= nice) {
      labelSeconds = nice;
      break;
    }
  }

  QFont plain = palette::mono(11);
  QFont bold = palette::mono(11, 600);
  qint64 lastBucket = std::numeric_limits<qint64>::min();
  int lastLabelX = -spacing;
  for (int i = first_; i < first_ + count_; ++i) {
    const QDateTime when = QDateTime::fromSecsSinceEpoch(candles_[i].time);
    const qint64 local = candles_[i].time + when.offsetFromUtc();
    const qint64 bucket = local / labelSeconds;
    if (bucket == lastBucket) continue;
    const bool first = lastBucket == std::numeric_limits<qint64>::min();
    lastBucket = bucket;
    const int x = xFor(i);
    // A label that would be cut off at either edge is left out, and so is a partial first period.
    if ((first && i > 0 && local % labelSeconds != 0) || x - lastLabelX < spacing - 8 || x < spacing / 2 || x > plot.right() - spacing / 4) continue;
    lastLabelX = x;
    painter.setPen(palette::kGrid);
    painter.drawLine(x, plot.top(), x, plot.bottom());
    const bool newDay = local % 86400 < barSeconds;
    const bool newHour = local % 3600 < barSeconds;
    QString text;
    if (barSeconds >= 86400 || (newDay && labelSeconds >= 3600)) {
      text = labelSeconds >= 2592000 ? when.toString(QStringLiteral("MMM yy")) : when.toString(QStringLiteral("dd MMM"));
    } else {
      text = when.toString(QStringLiteral("HH:mm"));
    }
    painter.setFont(newDay || (newHour && labelSeconds < 3600) ? bold : plain);
    painter.setPen(palette::kMute);
    painter.drawText(QRect(x - spacing / 2, axis.top(), spacing, axis.height()), Qt::AlignCenter, text);
  }
  painter.setFont(plain);
}

void CandleChart::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.fillRect(rect(), palette::kBackground);
  painter.setFont(palette::mono(11));

  const auto drawMessage = [&] {
    if (message_.isEmpty()) return;
    const QFont font = palette::mono(13);
    painter.setFont(font);
    const QFontMetrics metrics(font);
    const QRect text = metrics.boundingRect(QRect(0, 0, std::max(width() - 60, 100), height()), Qt::TextWordWrap, message_);
    const QRect box(width() - text.width() - 30, height() - text.height() - 44, text.width() + 18, text.height() + 10);
    painter.fillRect(box, palette::kRaise);
    painter.setPen(palette::kLine);
    painter.drawRect(box.adjusted(0, 0, -1, -1));
    painter.setPen(palette::kWarn);
    painter.drawText(box.adjusted(9, 5, -9, -5), Qt::TextWordWrap, message_);
  };

  if (candles_.isEmpty()) {
    painter.setPen(palette::kMute);
    painter.setFont(palette::mono(13));
    painter.drawText(rect().adjusted(16, 0, -16, 0), Qt::AlignCenter | Qt::TextWordWrap, emptyMessage_);
    drawMessage();
    return;
  }

  const QRect plot = plotRect();
  int volumeHeight = anyVolume_ ? static_cast<int>(plot.height() * kVolumeShare) : 0;
  QRect prices = plot.adjusted(0, 0, 0, -volumeHeight);
  QRect volumes(plot.left(), prices.bottom() + 1, plot.width(), volumeHeight);
  if (terminal_) {
    // The volume overlays the bottom of the price area, with the same margins as the browser chart.
    prices = QRect(plot.left(), plot.top() + static_cast<int>(plot.height() * kPriceTopMargin), plot.width(),
                   static_cast<int>(plot.height() * (1.0 - kPriceTopMargin - kPriceBottomMargin)));
    volumeHeight = anyVolume_ ? static_cast<int>(plot.height() * (1.0 - kVolumeTop)) : 0;
    volumes = QRect(plot.left(), plot.bottom() - volumeHeight + 1, plot.width(), volumeHeight);
  }

  // The vertical scale fits what is visible.
  double low = std::numeric_limits<double>::max();
  double high = std::numeric_limits<double>::lowest();
  double maxVolume = 0.0;
  for (int i = first_; i < first_ + count_; ++i) {
    low = std::min(low, candles_[i].low);
    high = std::max(high, candles_[i].high);
    if (candles_[i].hasVolume) maxVolume = std::max(maxVolume, candles_[i].volume);
  }
  if (high <= low) {  // Every visible price is the same.
    const double margin = std::max(std::abs(high) * 0.01, 1e-9);
    high += margin;
    low -= margin;
  }
  if (!terminal_) {
    const double padding = (high - low) * 0.05;
    low -= padding;
    high += padding;
  }
  const auto yFor = [&](double price) { return prices.bottom() - (price - low) / (high - low) * (prices.height() - 1); };
  const double barWidth = static_cast<double>(plot.width()) / (count_ + rightOffset());
  const auto xFor = [&](int index) { return plot.left() + (index - first_ + 0.5) * barWidth; };

  // Price grid and labels. The grid covers the whole plot, as the scale does in the browser chart.
  const double priceSpan = (high - low) * plot.height() / std::max(prices.height(), 1);
  const double step = niceStep(priceSpan, std::max(plot.height() / 60, 2));
  const double scaleBottom = low - (plot.bottom() - prices.bottom()) * (high - low) / std::max(prices.height(), 1);
  const double scaleTop = high + (prices.top() - plot.top()) * (high - low) / std::max(prices.height(), 1);
  for (double level = std::ceil(scaleBottom / step) * step; level <= scaleTop; level += step) {
    const int y = static_cast<int>(std::lround(yFor(level)));
    painter.setPen(terminal_ ? palette::kGrid : palette::kLine);
    painter.drawLine(plot.left(), y, plot.right(), y);
    // The last-price label takes priority over a grid label it would cover.
    const bool lastInView = terminal_ && first_ + count_ == candleCount();
    if (lastInView && std::abs(y - yFor(candles_.last().close)) < 18.0) continue;
    painter.setPen(palette::kMute);
    painter.drawText(QRect(plot.right() + 8, y - 10, kAxisWidth - 10, 20), Qt::AlignLeft | Qt::AlignVCenter, priceText(level, step));
  }

  paintTimeLabels(painter, plot, barWidth);

  // Axis borders.
  if (terminal_) {
    painter.setPen(palette::kLine);
    painter.drawLine(plot.right() + 1, plot.top(), plot.right() + 1, plot.bottom());
    painter.drawLine(plot.left(), plot.bottom() + 1, plot.right() + kAxisWidth, plot.bottom() + 1);
  }

  // Candles. Every bar is drawn, so a high or low is never lost when bars are narrower than a pixel.
  const int bodyWidth = std::max(1, static_cast<int>(barWidth * 0.7));
  painter.setClipRect(plot);
  for (int i = first_; i < first_ + count_; ++i) {
    const Candle& c = candles_[i];
    if (c.hasVolume && maxVolume > 0.0) {
      const bool rising = c.close >= c.open;
      const int x = static_cast<int>(std::lround(xFor(i)));
      const int barHeight = static_cast<int>(c.volume / maxVolume * (volumes.height() - 4));
      painter.fillRect(x - bodyWidth / 2, volumes.bottom() - barHeight, bodyWidth, barHeight, rising ? palette::kUpVolume : palette::kDownVolume);
    }
  }
  for (int i = first_; i < first_ + count_; ++i) {
    const Candle& c = candles_[i];
    const QColor color = c.close >= c.open ? palette::kUp : palette::kDown;
    const int x = static_cast<int>(std::lround(xFor(i)));
    painter.setPen(color);
    painter.drawLine(x, static_cast<int>(std::lround(yFor(c.high))), x, static_cast<int>(std::lround(yFor(c.low))));
    const int top = static_cast<int>(std::lround(yFor(std::max(c.open, c.close))));
    const int bottom = static_cast<int>(std::lround(yFor(std::min(c.open, c.close))));
    painter.fillRect(x - bodyWidth / 2, top, bodyWidth, std::max(bottom - top, 1), color);
  }

  if (terminal_) {
    // EMA 20.
    QPainterPath line;
    for (int i = first_; i < first_ + count_; ++i) {
      const QPointF point(xFor(i), yFor(ema_[i]));
      if (i == first_) line.moveTo(point);
      else line.lineTo(point);
    }
    painter.setRenderHint(QPainter::Antialiasing);
    painter.strokePath(line, QPen(palette::kAmber, 1.0));
    painter.setRenderHint(QPainter::Antialiasing, false);
  }

  painter.setClipRect(rect());

  const auto axisLabel = [&](double price, const QColor& fill, const QColor& text, const QString& prefix = {}) {
    const int y = static_cast<int>(std::lround(yFor(price)));
    const QRect box(plot.right() + 2, y - 9, kAxisWidth - 2, 18);
    painter.fillRect(box, fill);
    painter.setPen(text);
    painter.drawText(box.adjusted(6, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, prefix + priceText(price, step));
  };

  if (terminal_) {
    for (const double level : alerts_) {
      const int y = static_cast<int>(std::lround(yFor(level)));
      if (y < plot.top() || y > plot.bottom()) continue;
      painter.setPen(QPen(palette::kAmber, 1, Qt::DashLine));
      painter.drawLine(plot.left(), y, plot.right(), y);
      painter.setPen(palette::kAmber);
      painter.drawText(QRect(plot.right() - 60, y - 16, 56, 16), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("ALERT"));
      axisLabel(level, palette::kLine, palette::kAmber);
    }
    // The last price, while it is in view.
    const double last = candles_.last().close;
    const int y = static_cast<int>(std::lround(yFor(last)));
    if (first_ + count_ == candleCount() && y >= plot.top() && y <= plot.bottom()) {
      painter.setPen(QPen(palette::kAmber, 1, Qt::DotLine));
      painter.drawLine(plot.left(), y, plot.right(), y);
      axisLabel(last, palette::kAmber, Qt::black);
    }
  }

  // Crosshair.
  if (hovered_ >= first_ && hovered_ < first_ + count_) {
    const int x = static_cast<int>(std::lround(xFor(hovered_)));
    painter.setPen(QPen(terminal_ ? palette::kAmber : palette::kMute, 1, Qt::DashLine));
    painter.drawLine(x, plot.top(), x, plot.bottom());
    if (terminal_) {
      if (pointerY_ >= plot.top() && pointerY_ <= plot.bottom()) {
        painter.drawLine(plot.left(), pointerY_, plot.right(), pointerY_);
        const double price = low + (prices.bottom() - pointerY_) * (high - low) / std::max(prices.height() - 1, 1);
        const QRect box(plot.right() + 2, pointerY_ - 9, kAxisWidth - 2, 18);
        painter.fillRect(box, palette::kLine);
        painter.setPen(palette::kForeground);
        painter.drawText(box.adjusted(6, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, priceText(price, step));
      }
      const QString when = QDateTime::fromSecsSinceEpoch(candles_[hovered_].time).toString(QStringLiteral("dd MMM HH:mm"));
      const QRect box(std::clamp(x - 55, 0, std::max(width() - 110, 0)), plot.bottom() + 2, 110, kTimeAxisHeight - 3);
      painter.fillRect(box, palette::kLine);
      painter.setPen(palette::kForeground);
      painter.drawText(box, Qt::AlignCenter, when);
    }
  }

  if (!terminal_) {
    painter.setPen(palette::kForeground);
    const int shown = hovered_ >= 0 ? hovered_ : first_ + count_ - 1;
    painter.drawText(QRect(6, 0, width() - 12, kReadoutHeight), Qt::AlignLeft | Qt::AlignVCenter, readout(shown));
  }
  drawMessage();
}

void CandleChart::setHovered(int index) {
  if (index == hovered_) return;
  hovered_ = index;
  emit hoverChanged(index);
}

void CandleChart::wheelEvent(QWheelEvent* event) {
  if (candles_.isEmpty()) return;
  const QRect plot = plotRect();
  const double anchor = (event->position().x() - plot.left()) / plot.width();
  zoomBy(event->angleDelta().y() > 0 ? 1.25 : 0.8, anchor);
  event->accept();
}

void CandleChart::mousePressEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton) return;
  setFocus();
  dragging_ = true;
  dragStartX_ = static_cast<int>(event->position().x());
  dragStartFirst_ = first_;
}

void CandleChart::mouseMoveEvent(QMouseEvent* event) {
  const int x = static_cast<int>(event->position().x());
  if (dragging_ && count_ > 0) {
    userMovedView_ = true;
    first_ = dragStartFirst_ - static_cast<int>((x - dragStartX_) * static_cast<double>(count_ + rightOffset()) / plotRect().width());
    clampView();
  }
  pointerY_ = static_cast<int>(event->position().y());
  setHovered(indexAtX(x));
  update();
}

void CandleChart::mouseReleaseEvent(QMouseEvent*) { dragging_ = false; }

void CandleChart::leaveEvent(QEvent*) {
  setHovered(-1);
  update();
}

void CandleChart::keyPressEvent(QKeyEvent* event) {
  const int stepSize = std::max(1, count_ / 10);
  switch (event->key()) {
    case Qt::Key_Left:
      panBy(-stepSize);
      break;
    case Qt::Key_Right:
      panBy(stepSize);
      break;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
      zoomBy(1.25);
      break;
    case Qt::Key_Minus:
      zoomBy(0.8);
      break;
    case Qt::Key_Home:
      showAll();
      break;
    default:
      QWidget::keyPressEvent(event);
  }
}

}  // namespace rq
