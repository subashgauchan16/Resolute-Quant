#pragma once

#include <QRect>
#include <QVector>
#include <QWidget>

namespace rq {

struct Candle {
  qint64 time = 0;  // Seconds since 1970, UTC.
  double open = 0.0;
  double high = 0.0;
  double low = 0.0;
  double close = 0.0;
  double volume = 0.0;
  bool hasVolume = false;  // A missing volume is never drawn as zero.
};

// Draws candles and volume with QPainter. Qt Charts is avoided because of its licence.
// Candles sit side by side, so a time gap shows as a jump in the date axis, never as invented bars.
// Mouse wheel or + and - zoom, dragging or the arrow keys pan, and Home shows everything.
//
// The terminal style matches the browser terminal's chart: an EMA 20 line, a dotted last-price line with
// its label, alert lines, an amber crosshair, 8-pixel bars with space on the right, and local times.
// The plain style, used on the History page, has a text readout on top and shows UTC.
class CandleChart : public QWidget {
  Q_OBJECT

 public:
  explicit CandleChart(QWidget* parent = nullptr);

  void setTerminalStyle(bool terminal);
  // Oldest first. keepView holds the zoom, for a reload of the same pair and timeframe.
  void setCandles(QVector<Candle> candles, bool intraday, bool keepView = false);
  void clear(const QString& emptyMessage = {});
  // Replaces the newest candle, or adds a newer one, keeping the zoom. Stays on the newest candle
  // if it was showing, and otherwise leaves the view where the person moved it.
  void updateLatest(const Candle& candle);

  void setPriceDecimals(int decimals);
  void setAlertLevels(QVector<double> levels);
  void setMessage(const QString& message);  // A note in the corner, over any bars on show.

  int candleCount() const { return static_cast<int>(candles_.size()); }
  const Candle& candleAt(int index) const { return candles_[index]; }
  double emaAt(int index) const { return ema_[index]; }
  int firstVisible() const { return first_; }
  int visibleCount() const { return count_; }
  int hoveredIndex() const { return hovered_; }
  QString message() const { return message_; }
  QRect plotRect() const;

  void zoomBy(double factor, double anchorFraction = 1.0);  // Above 1 shows fewer candles.
  void panBy(int candles);
  void showAll();
  int indexAtX(int x) const;  // -1 if there is no candle at x.
  QString readout(int index) const;

 signals:
  void hoverChanged(int index);  // -1 when the pointer leaves the candles.

 protected:
  void paintEvent(QPaintEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void leaveEvent(QEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;

 private:
  void clampView();
  void applyDefaultView();
  void rebuildEma();
  void setHovered(int index);
  int rightOffset() const;
  QString priceText(double price, double step) const;
  void paintTimeLabels(QPainter& painter, const QRect& plot, double barWidth);

  QVector<Candle> candles_;
  QVector<double> ema_;
  QVector<double> alerts_;
  bool terminal_ = false;
  bool intraday_ = false;
  bool anyVolume_ = false;
  bool userMovedView_ = false;
  int priceDecimals_ = -1;
  QString emptyMessage_;
  QString message_;
  int first_ = 0;
  int count_ = 0;
  int hovered_ = -1;
  int pointerY_ = -1;
  bool dragging_ = false;
  int dragStartX_ = 0;
  int dragStartFirst_ = 0;
};

}  // namespace rq
