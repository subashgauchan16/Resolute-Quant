#include "app/CandleChart.h"

#include <QDateTime>
#include <QKeyEvent>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QTimeZone>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <limits>

namespace rq {

namespace {

constexpr int kMinVisible = 10;
constexpr int kAxisWidth = 84;
constexpr int kTimeAxisHeight = 24;
constexpr int kReadoutHeight = 26;
constexpr double kVolumeShare = 0.18;
constexpr int kMinLabelSpacing = 110;

const QColor kUp(0x2b, 0xd9, 0x8a);  // The same colours as the browser terminal.
const QColor kDown(0xff, 0x5a, 0x52);
const QColor kGrid(0x2a, 0x2c, 0x32);
const QColor kAxisText(0x9a, 0x9e, 0xa6);
const QColor kCrosshair(0x6c, 0x70, 0x78);
const QColor kReadout(0xd7, 0xd9, 0xdd);

QString exact(double value) { return QString::number(value, 'f', QLocale::FloatingPointShortest); }

QString timeText(qint64 seconds, bool intraday) {
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

void CandleChart::setCandles(QVector<Candle> candles, bool intraday) {
  candles_ = std::move(candles);
  intraday_ = intraday;
  anyVolume_ = std::any_of(candles_.cbegin(), candles_.cend(), [](const Candle& c) { return c.hasVolume; });
  emptyMessage_.clear();
  hovered_ = -1;
  showAll();
}

void CandleChart::clear(const QString& emptyMessage) {
  candles_.clear();
  anyVolume_ = false;
  emptyMessage_ = emptyMessage;
  first_ = count_ = 0;
  hovered_ = -1;
  update();
}

QRect CandleChart::plotRect() const {
  return QRect(0, kReadoutHeight, std::max(width() - kAxisWidth, 1), std::max(height() - kReadoutHeight - kTimeAxisHeight, 1));
}

void CandleChart::clampView() {
  const int total = candleCount();
  count_ = std::clamp(count_, std::min(kMinVisible, total), total);
  first_ = std::clamp(first_, 0, total - count_);
}

void CandleChart::showAll() {
  first_ = 0;
  count_ = candleCount();
  update();
}

void CandleChart::zoomBy(double factor, double anchorFraction) {
  if (candles_.isEmpty()) return;
  const double anchor = first_ + std::clamp(anchorFraction, 0.0, 1.0) * count_;
  const int newCount = std::clamp(static_cast<int>(std::lround(count_ / factor)), std::min(kMinVisible, candleCount()), candleCount());
  first_ = static_cast<int>(std::lround(anchor - std::clamp(anchorFraction, 0.0, 1.0) * newCount));
  count_ = newCount;
  clampView();
  update();
}

void CandleChart::panBy(int candles) {
  first_ += candles;
  clampView();
  update();
}

int CandleChart::indexAtX(int x) const {
  const QRect plot = plotRect();
  if (count_ <= 0 || x < plot.left() || x > plot.right()) return -1;
  const int index = first_ + static_cast<int>((x - plot.left()) * static_cast<double>(count_) / plot.width());
  return index < first_ + count_ ? index : -1;
}

QString CandleChart::readout(int index) const {
  if (index < 0 || index >= candleCount()) return {};
  const Candle& c = candles_[index];
  return QStringLiteral("%1 UTC  O %2  H %3  L %4  C %5  V %6")
      .arg(timeText(c.time, intraday_), exact(c.open), exact(c.high), exact(c.low), exact(c.close),
           c.hasVolume ? exact(c.volume) : QStringLiteral("not reported"));
}

void CandleChart::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.fillRect(rect(), QColor(0x10, 0x11, 0x14));

  if (candles_.isEmpty()) {
    painter.setPen(kAxisText);
    painter.drawText(rect().adjusted(16, 0, -16, 0), Qt::AlignCenter | Qt::TextWordWrap, emptyMessage_);
    return;
  }

  const QRect plot = plotRect();
  const int volumeHeight = anyVolume_ ? static_cast<int>(plot.height() * kVolumeShare) : 0;
  const QRect prices = plot.adjusted(0, 0, 0, -volumeHeight);
  const QRect volumes(plot.left(), prices.bottom() + 1, plot.width(), volumeHeight);

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
  const double padding = (high - low) * 0.05;
  low -= padding;
  high += padding;
  const auto yFor = [&](double price) { return prices.bottom() - (price - low) / (high - low) * (prices.height() - 1); };
  const double barWidth = static_cast<double>(plot.width()) / count_;
  const auto xFor = [&](int index) { return plot.left() + (index - first_ + 0.5) * barWidth; };

  // Price grid and labels.
  const double step = niceStep(high - low, std::max(prices.height() / 60, 2));
  painter.setFont(font());
  for (double level = std::ceil(low / step) * step; level <= high; level += step) {
    const int y = static_cast<int>(std::lround(yFor(level)));
    painter.setPen(kGrid);
    painter.drawLine(plot.left(), y, plot.right(), y);
    painter.setPen(kAxisText);
    painter.drawText(QRect(plot.right() + 6, y - 10, kAxisWidth - 8, 20), Qt::AlignLeft | Qt::AlignVCenter,
                     QLocale::c().toString(level, 'g', 8));
  }

  // Date labels.
  const int labelEvery = std::max(1, static_cast<int>(std::ceil(kMinLabelSpacing / barWidth)));
  painter.setPen(kAxisText);
  for (int i = first_ + labelEvery / 2; i < first_ + count_; i += labelEvery) {
    const int x = static_cast<int>(std::lround(xFor(i)));
    painter.setPen(kGrid);
    painter.drawLine(x, plot.top(), x, plot.bottom());
    painter.setPen(kAxisText);
    painter.drawText(QRect(x - kMinLabelSpacing / 2, plot.bottom() + 2, kMinLabelSpacing, kTimeAxisHeight - 2),
                     Qt::AlignCenter, timeText(candles_[i].time, intraday_));
  }

  // Candles. Every bar is drawn, so a high or low is never lost when bars are narrower than a pixel.
  const int bodyWidth = std::max(1, static_cast<int>(barWidth * 0.7));
  painter.setClipRect(plot);
  for (int i = first_; i < first_ + count_; ++i) {
    const Candle& c = candles_[i];
    const QColor color = c.close >= c.open ? kUp : kDown;
    const int x = static_cast<int>(std::lround(xFor(i)));
    painter.setPen(color);
    painter.drawLine(x, static_cast<int>(std::lround(yFor(c.high))), x, static_cast<int>(std::lround(yFor(c.low))));
    const int top = static_cast<int>(std::lround(yFor(std::max(c.open, c.close))));
    const int bottom = static_cast<int>(std::lround(yFor(std::min(c.open, c.close))));
    painter.fillRect(x - bodyWidth / 2, top, bodyWidth, std::max(bottom - top, 1), color);

    if (c.hasVolume && maxVolume > 0.0) {
      const int barHeight = static_cast<int>(c.volume / maxVolume * (volumes.height() - 4));
      QColor faded = color;
      faded.setAlpha(110);
      painter.fillRect(x - bodyWidth / 2, volumes.bottom() - barHeight, bodyWidth, barHeight, faded);
    }
  }

  if (hovered_ >= first_ && hovered_ < first_ + count_) {
    painter.setPen(QPen(kCrosshair, 1, Qt::DashLine));
    const int x = static_cast<int>(std::lround(xFor(hovered_)));
    painter.drawLine(x, plot.top(), x, plot.bottom());
  }
  painter.setClipping(false);

  painter.setPen(kReadout);
  const int shown = hovered_ >= 0 ? hovered_ : first_ + count_ - 1;
  painter.drawText(QRect(6, 0, width() - 12, kReadoutHeight), Qt::AlignLeft | Qt::AlignVCenter, readout(shown));
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
    first_ = dragStartFirst_ - static_cast<int>((x - dragStartX_) * static_cast<double>(count_) / plotRect().width());
    clampView();
  }
  hovered_ = indexAtX(x);
  update();
}

void CandleChart::mouseReleaseEvent(QMouseEvent*) { dragging_ = false; }

void CandleChart::leaveEvent(QEvent*) {
  hovered_ = -1;
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
