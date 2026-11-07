#pragma once

#include <QString>

namespace rq {

// Loads the terminal's bundled fonts. Call once, after the application object exists.
void loadFonts();

// The application stylesheet, using the browser terminal's colours.
QString applicationStyleSheet();

// True if Windows is set to reduce animation, which the logo and shine respect.
bool reducedMotion();

}  // namespace rq
