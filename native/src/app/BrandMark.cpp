#include "app/BrandMark.h"

#include <QKeyEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QRadialGradient>
#include <QRandomGenerator>
#include <QTimer>
#include <QVector>
#include <algorithm>
#include <cmath>

#include "app/Palette.h"
#include "app/Theme.h"

namespace rq {

namespace {

constexpr int kFrameMs = 33;
constexpr int kAutoCloseMs = 3600;
constexpr int kCloseFadeMs = 450;
constexpr int kDustCount = 16;
constexpr double kTiltDegrees = 4.0;
constexpr double kLogoWidth = 1672.0;
constexpr double kLogoHeight = 941.0;

const QColor kGold(0xd9, 0xa9, 0x3f);
const QColor kGoldHigh(0xff, 0xf3, 0xc9);
const QColor kSilver(0xa9, 0xac, 0xb2);
const QColor kSilverHigh(0xff, 0xff, 0xff);

// The CSS easing cubic-bezier(.45,.05,.55,.95), used by the two sweeps.
double easeInOut(double t) {
  double low = 0.0, high = 1.0;
  for (int i = 0; i < 20; ++i) {
    const double u = (low + high) / 2.0;
    const double x = 3 * (1 - u) * (1 - u) * u * 0.45 + 3 * (1 - u) * u * u * 0.55 + u * u * u;
    (x < t ? low : high) = u;
  }
  const double u = (low + high) / 2.0;
  return 3 * (1 - u) * (1 - u) * u * 0.05 + 3 * (1 - u) * u * u * 0.95 + u * u * u;
}

// One pass takes up `share` of the cycle, then it rests until the cycle ends.
double sweepFraction(double seconds, double period, double share, double delay = 0.0) {
  const double phase = std::fmod(std::max(seconds - delay, 0.0), period) / period;
  return phase >= share ? 1.0 : easeInOut(phase / share);
}

// The page uses colour-dodge, which leaves black alone so the light only lands on the metal. Qt's version
// of that blend turns faint noise in dark pixels into red specks, so this gets the same effect another way:
// it adds the image's own pixels back through a moving band, which brightens metal and leaves black black.
void addSweep(QImage& image, double fraction, double travel, double strength, double shoulderShare, double halfWidth) {
  const qreal ratio = image.devicePixelRatio();
  const QRectF area(QPointF(0, 0), QSizeF(image.size()) / ratio);
  // A band at 105 degrees across the area, moved from -travel to +travel of the overscan width.
  const double width = area.width() * 2.2;
  const QPointF centre(area.center().x() + (-travel + 2.0 * travel * fraction) * width, area.center().y());
  const double angle = 105.0 * M_PI / 180.0;
  const QPointF direction(std::sin(angle), -std::cos(angle));
  const double length = std::abs(width * direction.x()) + std::abs(area.height() * 3.0 * direction.y());
  QLinearGradient gradient(centre - direction * (length / 2), centre + direction * (length / 2));
  const QColor peak(255, 255, 255, 255);
  QColor shoulder = peak;
  shoulder.setAlphaF(shoulderShare);
  gradient.setColorAt(std::max(0.5 - halfWidth, 0.0), Qt::transparent);
  gradient.setColorAt(std::max(0.5 - halfWidth * 0.3, 0.0), shoulder);
  gradient.setColorAt(0.5, peak);
  gradient.setColorAt(std::min(0.5 + halfWidth * 0.3, 1.0), shoulder);
  gradient.setColorAt(std::min(0.5 + halfWidth, 1.0), Qt::transparent);

  QImage band(image.size(), QImage::Format_ARGB32_Premultiplied);
  band.setDevicePixelRatio(ratio);
  band.fill(Qt::transparent);
  {
    QPainter bandPainter(&band);
    bandPainter.fillRect(area, gradient);
    bandPainter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    bandPainter.drawImage(QPointF(0, 0), image);
  }
  QPainter painter(&image);
  painter.setCompositionMode(QPainter::CompositionMode_Plus);
  painter.setOpacity(strength);
  painter.drawImage(QPointF(0, 0), band);
}

void paintWord(QPainter& painter, const QString& text, const QPointF& baseline, const QFont& font, const QColor& base, const QColor& high, double seconds, double delay) {
  // background-size 260%: a highlight from 30% to 70%, moving from 130% to -30% over 55% of 5 seconds.
  const double phase = std::fmod(std::max(seconds - delay, 0.0), 5.0) / 5.0;
  const double position = phase >= 0.55 ? -0.3 : 1.3 - 1.6 * (phase / 0.55);
  const QFontMetricsF metrics(font);
  const double width = metrics.horizontalAdvance(text);
  const double gradientWidth = width * 2.6;
  const double left = baseline.x() + (width - gradientWidth) * position;
  QLinearGradient gradient(left, 0, left + gradientWidth, 0);
  gradient.setColorAt(0.3, base);
  gradient.setColorAt(0.5, high);
  gradient.setColorAt(0.7, base);
  QPainterPath path;
  path.addText(baseline, font, text);
  painter.fillPath(path, gradient);
}

}  // namespace

BrandMark::BrandMark(QWidget* parent) : QWidget(parent), mark_(QStringLiteral(":/native/assets/img/rq-mark.png")), timer_(new QTimer(this)) {
  setObjectName(QStringLiteral("brand"));
  setCursor(Qt::PointingHandCursor);
  setToolTip(QStringLiteral("Show the Resolute Quant logo"));
  setAccessibleName(QStringLiteral("Resolute Quant. Show the logo."));
  const QFont font = palette::label(14, 600, 0.16);
  const QFontMetrics metrics(font);
  setFixedSize(55 + 9 + metrics.horizontalAdvance(QStringLiteral("RESOLUTE QUANT")) + 6, 42);
  connect(timer_, &QTimer::timeout, this, qOverload<>(&QWidget::update));
  if (!reducedMotion()) timer_->start(kFrameMs);
  clock_.start();
}

void BrandMark::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setRenderHint(QPainter::SmoothPixmapTransform);
  painter.fillRect(rect(), palette::kBackground);
  const double seconds = clock_.elapsed() / 1000.0;
  const bool motion = timer_->isActive();

  // background-size: contain, in a 55 by 34 box, with the sweep clipped to the box.
  const QRect box(0, (height() - 34) / 2, 55, 34);
  const QSize fitted = mark_.size().scaled(box.size(), Qt::KeepAspectRatio);
  const QRect target(box.center().x() - fitted.width() / 2 + 1, box.center().y() - fitted.height() / 2 + 1, fitted.width(), fitted.height());
  // Layered so the dodge only touches the logo's own black box.
  QImage layer(box.size() * devicePixelRatioF(), QImage::Format_ARGB32_Premultiplied);
  layer.setDevicePixelRatio(devicePixelRatioF());
  layer.fill(Qt::black);
  {
    QPainter markPainter(&layer);
    markPainter.setRenderHint(QPainter::SmoothPixmapTransform);
    markPainter.drawPixmap(target.translated(-box.topLeft()), mark_);
  }
  if (motion) addSweep(layer, sweepFraction(seconds, 5.0, 0.55), 0.6, 1.0, 0.3, 0.06);
  painter.drawImage(box.topLeft(), layer);
  if (hovering_) {
    painter.save();
    painter.setCompositionMode(QPainter::CompositionMode_Plus);
    painter.drawImage(box.topLeft(), layer);  // Close to the page's brightness(1.2) on hover.
    painter.restore();
  }

  const QFont font = palette::label(14, 600, 0.16);
  const QFontMetricsF metrics(font);
  const double baseline = height() / 2.0 + (metrics.ascent() - metrics.descent()) / 2.0;
  const double left = 55.0 + 9.0;
  const double after = motion ? seconds : 2.2;  // A still frame sits at the middle of the shimmer.
  paintWord(painter, QStringLiteral("RESOLUTE"), QPointF(left, baseline), font, kGold, kGoldHigh, after, 0.0);
  paintWord(painter, QStringLiteral("QUANT"), QPointF(left + metrics.horizontalAdvance(QStringLiteral("RESOLUTE ")), baseline), font, kSilver, kSilverHigh, after, 0.25);
}

void BrandMark::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) emit clicked();
}

void BrandMark::enterEvent(QEnterEvent*) {
  hovering_ = true;
  update();
}

void BrandMark::leaveEvent(QEvent*) {
  hovering_ = false;
  update();
}

LogoIntro::LogoIntro(QWidget* parent)
    : QWidget(parent), logo_(QStringLiteral(":/native/assets/img/resolute-quant-logo.png")), timer_(new QTimer(this)), autoClose_(new QTimer(this)) {
  setObjectName(QStringLiteral("logoIntro"));
  setCursor(Qt::PointingHandCursor);
  setMouseTracking(true);
  setFocusPolicy(Qt::StrongFocus);
  hide();
  connect(timer_, &QTimer::timeout, this, &LogoIntro::tick);
  autoClose_->setSingleShot(true);
  connect(autoClose_, &QTimer::timeout, this, &LogoIntro::close);
}

void LogoIntro::open(bool closesItself) {
  closing_ = false;
  dust_.clear();
  auto* random = QRandomGenerator::global();
  for (int i = 0; i < kDustCount; ++i) {
    dust_.push_back({random->bounded(1.0), 7.0 + random->bounded(8.0), -random->bounded(12.0)});
  }
  autoClose_->stop();
  setGeometry(parentWidget()->rect());
  raise();
  show();
  setFocus();
  clock_.start();
  if (!reducedMotion()) timer_->start(kFrameMs);
  if (closesItself) autoClose_->start(kAutoCloseMs);
}

void LogoIntro::close() {
  if (!isVisible() || closing_) return;
  autoClose_->stop();
  closing_ = true;
  closeClock_.start();
  timer_->start(kFrameMs);  // The fade runs even when other motion is off.
}

void LogoIntro::tick() {
  if (closing_ && closeClock_.elapsed() >= kCloseFadeMs) {
    timer_->stop();
    closing_ = false;
    hide();
    if (parentWidget() != nullptr) parentWidget()->setFocus();
    return;
  }
  update();
}

void LogoIntro::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setRenderHint(QPainter::SmoothPixmapTransform);
  if (closing_) painter.setOpacity(1.0 - std::min(closeClock_.elapsed() / static_cast<double>(kCloseFadeMs), 1.0));
  const double seconds = clock_.elapsed() / 1000.0;
  const bool motion = !reducedMotion();

  // radial-gradient(ellipse 70% 60% at 50% 42%, #191307, #070503 45%, #000 75%)
  painter.fillRect(rect(), Qt::black);
  painter.save();
  painter.translate(width() * 0.5, height() * 0.42);
  painter.scale(width() * 0.7, height() * 0.6);
  QRadialGradient background(QPointF(0, 0), 1.0);
  background.setColorAt(0.0, QColor(0x19, 0x13, 0x07));
  background.setColorAt(0.45, QColor(0x07, 0x05, 0x03));
  background.setColorAt(0.75, Qt::black);
  background.setColorAt(1.0, Qt::black);
  painter.fillRect(QRectF(-2, -2, 4, 4), background);
  painter.restore();

  // width: min(92vw, (100vh - 110px) * 1.777), kept at the logo's own shape.
  const double stageWidth = std::min(width() * 0.92, (height() - 110) * 1.777);
  const double stageHeight = stageWidth * kLogoHeight / kLogoWidth;
  const QFont noteFont = palette::mono(12);
  const double noteHeight = 44.0;
  const double top = std::max((height() - stageHeight - noteHeight - 16.0) / 2.0, 0.0);
  const QRectF stage((width() - stageWidth) / 2.0, top, stageWidth, stageHeight);

  // Drawn to its own layer, so the light can dodge the logo and leave the page's backdrop alone.
  const qreal ratio = devicePixelRatioF();
  QImage layer((stage.size() * ratio).toSize(), QImage::Format_ARGB32_Premultiplied);
  layer.setDevicePixelRatio(ratio);
  layer.fill(Qt::black);
  {
    QPainter stagePainter(&layer);
    stagePainter.setRenderHint(QPainter::Antialiasing);
    stagePainter.setRenderHint(QPainter::SmoothPixmapTransform);
    const QRectF area(QPointF(0, 0), stage.size());
    const auto applyBreathing = [&] {
      const double breathe = 1.0 + 0.022 * (0.5 - 0.5 * std::cos(seconds * 2.0 * M_PI / 9.0));
      stagePainter.translate(area.center());
      stagePainter.scale(breathe, breathe);
      stagePainter.translate(-area.center());
    };
    if (motion) applyBreathing();
    stagePainter.drawPixmap(area.toRect(), logo_);
    if (motion) {
      stagePainter.end();
      addSweep(layer, sweepFraction(seconds, 4.6, 0.62), 0.55, 1.0, 0.35, 0.05);
      addSweep(layer, sweepFraction(seconds, 4.6, 0.62, 0.55), 0.55, 0.45, 0.35, 0.05);
      stagePainter.begin(&layer);
      stagePainter.setRenderHint(QPainter::Antialiasing);
      applyBreathing();

      // The spark running round the logo's ring: one short dash along the page's own path.
      stagePainter.save();
      stagePainter.setCompositionMode(QPainter::CompositionMode_Screen);
      stagePainter.scale(area.width() / kLogoWidth, area.height() / kLogoHeight);
      QPainterPath orbit(QPointF(838, 238));
      orbit.cubicTo(960, 170, 1180, 110, 1245, 140);
      orbit.cubicTo(1262, 150, 1266, 180, 1262, 205);
      orbit.cubicTo(1262, 290, 1040, 400, 760, 480);
      orbit.cubicTo(590, 530, 440, 565, 437, 525);
      orbit.cubicTo(436, 495, 490, 430, 555, 398);
      const double phase = std::fmod(seconds, 4.6) / 4.6;
      const double head = phase * 1.07;  // Dash offset runs from 1070 to 0 on a path of length 1000.
      const double dash = 0.07;
      const double from = std::clamp(head - dash, 0.0, 1.0);
      const double to = std::clamp(head, 0.0, 1.0);
      if (to > from) {
        QPainterPath piece(orbit.pointAtPercent(from));
        for (int i = 1; i <= 16; ++i) piece.lineTo(orbit.pointAtPercent(from + (to - from) * i / 16.0));
        for (const auto& [strokeWidth, colour] : {std::pair{19.0, QColor(255, 201, 92, 50)}, std::pair{11.0, QColor(255, 201, 92, 110)}, std::pair{5.0, QColor(255, 243, 207)}}) {
          stagePainter.strokePath(piece, QPen(colour, strokeWidth, Qt::SolidLine, Qt::RoundCap));
        }
      }
      stagePainter.restore();

      // The ember at the top of the ring, pulsing every 2.3 seconds.
      const double pulse = 0.5 - 0.5 * std::cos(seconds * 2.0 * M_PI / 2.3);
      const double emberRadius = area.width() * 0.16 / 2.0 * (0.82 + 0.32 * pulse);
      const QPointF emberCentre(area.width() * 0.75, area.height() * 0.152);
      QRadialGradient ember(emberCentre, emberRadius);
      ember.setColorAt(0.0, QColor(255, 226, 150, 242));
      ember.setColorAt(0.18, QColor(255, 170, 50, 107));
      ember.setColorAt(0.42, QColor(255, 150, 30, 26));
      ember.setColorAt(0.66, Qt::transparent);
      stagePainter.save();
      stagePainter.setCompositionMode(QPainter::CompositionMode_Screen);
      stagePainter.setOpacity(0.55 + 0.45 * pulse);
      stagePainter.fillRect(QRectF(emberCentre - QPointF(emberRadius, emberRadius), QSizeF(emberRadius * 2, emberRadius * 2)), ember);
      stagePainter.restore();
    }
  }

  // The logo leans gently towards the pointer.
  painter.save();
  if (motion) {
    QTransform tilt;
    tilt.translate(stage.center().x(), stage.center().y());
    tilt.rotate((pointer_.x() - 0.5) * kTiltDegrees, Qt::YAxis, 1400.0);
    tilt.rotate((0.5 - pointer_.y()) * kTiltDegrees, Qt::XAxis, 1400.0);
    tilt.translate(-stage.center().x(), -stage.center().y());
    painter.setTransform(tilt, true);
  }
  // Screen blend, so the logo's black box melts into the warm backdrop.
  painter.setCompositionMode(QPainter::CompositionMode_Screen);
  painter.drawImage(stage, layer);
  painter.restore();

  // Rising dust.
  if (motion) {
    painter.setPen(Qt::NoPen);
    for (const Dust& speck : dust_) {
      const double t = std::fmod(std::fmod(seconds - speck.delay, speck.duration) + speck.duration, speck.duration) / speck.duration;
      const double alpha = t < 0.12 ? t / 0.12 * 0.85 : t < 0.85 ? 0.85 - (t - 0.12) / 0.73 * 0.45 : 0.4 * (1.0 - (t - 0.85) / 0.15);
      const QPointF position(speck.left * width(), height() + 8.0 - t * height() * 0.92);
      QRadialGradient glow(position, 8.0);
      glow.setColorAt(0.0, QColor(255, 183, 51, static_cast<int>(alpha * 170)));
      glow.setColorAt(1.0, Qt::transparent);
      painter.setBrush(glow);
      painter.drawEllipse(position, 8.0, 8.0);
      painter.setBrush(QColor(255, 220, 140, static_cast<int>(alpha * 255)));
      painter.drawEllipse(position, 1.5, 1.5);
    }
  }

  // The note below the logo.
  const QRectF noteArea(0, stage.bottom() + 8.0, width(), noteHeight);
  QFont title = palette::mono(12, 500);
  title.setLetterSpacing(QFont::AbsoluteSpacing, 0.72);
  painter.setFont(title);
  painter.setPen(palette::kForeground);
  painter.drawText(noteArea.adjusted(0, 0, 0, -noteHeight / 2), Qt::AlignCenter, QStringLiteral("© 2026 RESOLUTE QUANT | SG | subashgauchan16"));
  painter.setFont(noteFont);
  painter.setPen(palette::kMute);
  painter.drawText(noteArea.adjusted(0, noteHeight / 2, 0, 0), Qt::AlignCenter, QStringLiteral("Research display only. Places no orders. Click or press any key to enter."));
}

void LogoIntro::mousePressEvent(QMouseEvent*) { close(); }

void LogoIntro::mouseMoveEvent(QMouseEvent* event) {
  pointer_ = QPointF(event->position().x() / std::max(width(), 1), event->position().y() / std::max(height(), 1));
}

void LogoIntro::keyPressEvent(QKeyEvent*) { close(); }

}  // namespace rq
