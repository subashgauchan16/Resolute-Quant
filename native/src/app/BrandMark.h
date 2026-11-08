#pragma once

#include <QElapsedTimer>
#include <QPixmap>
#include <QWidget>

class QTimer;

namespace rq {

// The header brand: the RQ mark with its light sweep, and the two shimmering words. Click opens the intro.
class BrandMark : public QWidget {
  Q_OBJECT

 public:
  explicit BrandMark(QWidget* parent = nullptr);

 signals:
  void clicked();

 protected:
  void paintEvent(QPaintEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void enterEvent(QEnterEvent* event) override;
  void leaveEvent(QEvent* event) override;

 private:
  QPixmap mark_;
  QTimer* timer_;
  QElapsedTimer clock_;
  bool hovering_ = false;
};

// The full-screen logo intro: breathing logo, sweeping light, orbiting spark, ember and rising dust.
// It closes on a click or key, or by itself when opened automatically.
class LogoIntro : public QWidget {
  Q_OBJECT

 public:
  explicit LogoIntro(QWidget* parent);

  void open(bool closesItself);
  void close();
  bool isOpen() const { return isVisible() && !closing_; }

 protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;

 private:
  struct Dust {
    double left;
    double duration;
    double delay;
  };

  void tick();

  QPixmap logo_;
  QTimer* timer_;
  QTimer* autoClose_;
  QElapsedTimer clock_;
  QElapsedTimer closeClock_;
  QVector<Dust> dust_;
  QPointF pointer_{0.5, 0.5};
  bool closing_ = false;
};

}  // namespace rq
