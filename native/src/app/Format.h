#pragma once

#include <QString>
#include <optional>

// Number and time formatting, ported from the browser terminal's assets/js/chart/helper.js.
namespace rq::format {

using Number = std::optional<double>;

inline constexpr auto kNone = "-";

int decimalsFor(double price);                         // dp()
QString number(Number value, int decimals);            // num(): en-GB grouping.
QString price(Number value);                           // px()
QString size(double value);                            // sz()
QString compact(Number value);                         // big(): 1.2K, 3.4M and so on.
QString percentArrow(Number value);                    // pctTxt()
QString clockTime(qint64 milliseconds);                // hms(): local time.
QString currencySymbol(const QString& code);           // ccySymbol()
QString marketState(const QString& yahooState);        // STATE
int decimalsForSymbol(bool isMarket, const QString& symbol, double value);  // dpFor()
QString symbolPrice(bool isMarket, const QString& symbol, Number value);    // pxs()
QString friendlyName(const QString& symbol);           // FRIENDLY, or the symbol itself.
QString timeframeLabel(const QString& name);           // granLabel(): 5M shows as 5m, 1MO stays.

}  // namespace rq::format
