#pragma once

#include <QColor>
#include <QFont>

// The browser terminal's design values, from assets/css/style.css and brand.css.
namespace rq::palette {

inline const QColor kBackground(0x00, 0x00, 0x00);
inline const QColor kPanel(0x07, 0x08, 0x0a);
inline const QColor kRaise(0x10, 0x12, 0x16);
inline const QColor kLine(0x23, 0x26, 0x2c);
inline const QColor kRowLine(0x15, 0x17, 0x1b);
inline const QColor kGrid(0x16, 0x18, 0x1c);
inline const QColor kForeground(0xe9, 0xe6, 0xdf);
inline const QColor kMute(0x8d, 0x90, 0x99);
inline const QColor kAmber(0xff, 0xb0, 0x00);
inline const QColor kUp(0x2b, 0xd9, 0x8a);
inline const QColor kDown(0xff, 0x5a, 0x52);
inline const QColor kWarn(0xff, 0xd2, 0x4a);
inline const QColor kUpBackground(43, 217, 138, 36);    // 14% opacity.
inline const QColor kDownBackground(255, 90, 82, 36);
inline const QColor kUpVolume(43, 217, 138, 97);        // 38% opacity.
inline const QColor kDownVolume(255, 90, 82, 97);

// Pixel sizes match the stylesheet. Weights are 400, 500 or 600 for mono, and 500 or 600 for labels.
QFont mono(int pixelSize, int weight = 400);
QFont label(int pixelSize, int weight = 600, double letterSpacingEm = 0.0);

}  // namespace rq::palette
