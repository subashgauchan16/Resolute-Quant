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
class CandleChart : public QWidget {
  Q_OBJECT

 public:
  explicit CandleChart(QWidget* parent = nullptr);

  void setCandles(QVector<Candle> candles, bool intraday);  // Oldest first.
  void clear(const QString& emptyMessage = {});
  // Replaces the newest candle, or adds a newer one, keeping the zoom. Stays on the newest candle
  // if it was showing, and otherwise leaves the view where the person moved it.
  void updateLatest(const Candle& candle);

  int candleCount() const { return static_cast<int>(candles_.size()); }
  int firstVisible() const { return first_; }
  int visibleCount() const { return count_; }
  int hoveredIndex() const { return hovered_; }
  QRect plotRect() const;

  void zoomBy(double factor, double anchorFraction = 1.0);  // Above 1 shows fewer candles.
  void panBy(int candles);
  void showAll();
  int indexAtX(int x) const;  // -1 if there is no candle at x.
  QString readout(int index) const;

 protected:
  void paintEvent(QPaintEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void leaveEvent(QEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;

 private:
  void clampView();

  QVector<Candle> candles_;
  bool intraday_ = false;
  bool anyVolume_ = false;
  QString emptyMessage_;
  int first_ = 0;
  int count_ = 0;
  int hovered_ = -1;
  bool dragging_ = false;
  int dragStartX_ = 0;
  int dragStartFirst_ = 0;
};

}  // namespace rq
