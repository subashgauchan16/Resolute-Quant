#include "app/Theme.h"

#include <windows.h>

#include <QApplication>
#include <QFontDatabase>

#include "app/Palette.h"

namespace rq {

namespace palette {

QFont mono(int pixelSize, int weight) {
  QFont font(QStringLiteral("IBM Plex Mono"));
  font.setPixelSize(pixelSize);
  font.setWeight(static_cast<QFont::Weight>(weight));
  font.setStyleHint(QFont::Monospace);
  return font;
}

QFont label(int pixelSize, int weight, double letterSpacingEm) {
  QFont font(QStringLiteral("IBM Plex Sans Condensed"));
  font.setPixelSize(pixelSize);
  font.setWeight(static_cast<QFont::Weight>(weight));
  if (letterSpacingEm != 0.0) font.setLetterSpacing(QFont::AbsoluteSpacing, letterSpacingEm * pixelSize);
  return font;
}

}  // namespace palette

void loadFonts() {
  for (const char* file : {"IBMPlexMono-Regular", "IBMPlexMono-Medium", "IBMPlexMono-SemiBold", "IBMPlexSansCondensed-Medium", "IBMPlexSansCondensed-SemiBold"}) {
    QFontDatabase::addApplicationFont(QStringLiteral(":/native/assets/fonts/%1.ttf").arg(QLatin1String(file)));
  }
  // Set here, not in the stylesheet, because a stylesheet font size would override every widget's own.
  // In points, because Qt's stylesheet engine warns about an application font sized in pixels. 13px is 9.75pt.
  QFont font(QStringLiteral("IBM Plex Mono"));
  font.setPointSizeF(9.75);
  font.setStyleHint(QFont::Monospace);
  QApplication::setFont(font);
}

bool reducedMotion() {
  BOOL animations = TRUE;
  SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
  return animations == FALSE;
}

QString applicationStyleSheet() {
  return QStringLiteral(R"(
    QWidget { background: #000000; color: #e9e6df; }
    QToolTip { background: #101216; color: #e9e6df; border: 1px solid #23262c; }
    QLabel { background: transparent; }
    QLabel#pageTitle { color: #ffb000; font-family: "IBM Plex Sans Condensed"; font-size: 18px; font-weight: 600; }
    QLabel#muted { color: #8d9099; font-size: 12px; }
    QGroupBox { border: 1px solid #23262c; margin-top: 14px; padding: 10px; }
    QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: #ffb000; }
    QPushButton { background: transparent; color: #8d9099; border: 1px solid #23262c; padding: 5px 9px; }
    QPushButton:hover, QPushButton:focus { color: #ffb000; border-color: #ffb000; }
    QPushButton:disabled { color: #4a4d55; border-color: #15171b; }
    QPushButton#interval { border: 1px solid transparent; padding: 3px 8px; }
    QPushButton#interval:hover { color: #e9e6df; }
    QPushButton#interval:checked { background: #ffb000; color: #000000; font-weight: 600; }
    QPushButton#linkButton { border: none; padding: 0; color: #000000; text-decoration: underline; font-weight: 600; }
    QLineEdit, QPlainTextEdit { background: #07080a; border: 1px solid #23262c; padding: 4px; }
    QLineEdit:focus, QPlainTextEdit:focus { border-color: #ffb000; }
    QComboBox, QDateEdit { background: #07080a; border: 1px solid #23262c; padding: 4px; min-width: 70px; min-height: 18px; }
    QDateEdit { min-width: 100px; }
    QComboBox:focus, QDateEdit:focus { border-color: #ffb000; }
    QComboBox QAbstractItemView { background: #07080a; selection-background-color: #101216; selection-color: #ffb000; }
    QTableView { outline: 0; background: #000000; gridline-color: #15171b; border: 1px solid #23262c; selection-background-color: #101216; selection-color: #ffb000; }
    QTableView::item { border: none; padding: 0 4px; }
    QTableView::item:selected { background: #101216; color: #ffb000; border: none; }
    QHeaderView::section { background: #07080a; color: #8d9099; border: none; border-bottom: 1px solid #23262c; padding: 4px; font-size: 11px; }
    QSplitter::handle { background: #23262c; height: 1px; }
    QScrollArea { border: none; }
    QScrollBar:vertical { background: #101216; width: 10px; margin: 0; }
    QScrollBar:horizontal { background: #101216; height: 10px; margin: 0; }
    QScrollBar::handle { background: #4a4d55; min-height: 24px; min-width: 24px; }
    QScrollBar::handle:hover { background: #8d9099; }
    QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
    QScrollBar::add-page, QScrollBar::sub-page { background: none; }
    QProgressBar { background: #101216; border: none; text-align: center; color: #e9e6df; }
    QProgressBar::chunk { background: #ffb000; }
  )");
}

}  // namespace rq
