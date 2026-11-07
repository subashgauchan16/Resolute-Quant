#include "app/Format.h"

#include <QDateTime>
#include <QLocale>
#include <QMap>
#include <QRegularExpression>
#include <cmath>

namespace rq::format {

int decimalsFor(double price) { return price >= 100 ? 2 : price >= 1 ? 4 : price >= 0.01 ? 5 : 7; }

QString number(Number value, int decimals) {
  if (!value || !std::isfinite(*value)) return QString::fromLatin1(kNone);
  static const QLocale locale(QLocale::English, QLocale::UnitedKingdom);
  return locale.toString(*value, 'f', decimals);
}

QString price(Number value) {
  if (!value) return QString::fromLatin1(kNone);
  return number(value, decimalsFor(std::abs(*value)));
}

QString size(double value) { return number(value, value >= 1000 ? 0 : value >= 10 ? 2 : value >= 0.01 ? 4 : 6); }

QString compact(Number value) {
  if (!value) return QString::fromLatin1(kNone);
  const double v = *value;
  if (v >= 1e12) return number(v / 1e12, 2) + QLatin1Char('T');
  if (v >= 1e9) return number(v / 1e9, 2) + QLatin1Char('B');
  if (v >= 1e6) return number(v / 1e6, 2) + QLatin1Char('M');
  if (v >= 1e3) return number(v / 1e3, 1) + QLatin1Char('K');
  return number(v, 2);
}

QString percentArrow(Number value) {
  if (!value) return QString::fromLatin1(kNone);
  const QString arrow = *value > 0 ? QStringLiteral("▲ ") : *value < 0 ? QStringLiteral("▼ ") : QString();
  return arrow + number(std::abs(*value), 2) + QLatin1Char('%');
}

QString clockTime(qint64 milliseconds) { return QDateTime::fromMSecsSinceEpoch(milliseconds).toString(QStringLiteral("HH:mm:ss")); }

QString currencySymbol(const QString& code) {
  if (code == QLatin1String("USD")) return QStringLiteral("$");
  if (code == QLatin1String("GBP")) return QStringLiteral("£");
  if (code == QLatin1String("EUR")) return QStringLiteral("€");
  return {};
}

QString marketState(const QString& yahooState) {
  static const QMap<QString, QString> states{{QStringLiteral("REGULAR"), QStringLiteral("OPEN")},
                                             {QStringLiteral("PRE"), QStringLiteral("PRE-MARKET")},
                                             {QStringLiteral("PREPRE"), QStringLiteral("CLOSED")},
                                             {QStringLiteral("POST"), QStringLiteral("AFTER HOURS")},
                                             {QStringLiteral("POSTPOST"), QStringLiteral("CLOSED")},
                                             {QStringLiteral("CLOSED"), QStringLiteral("CLOSED")}};
  return states.value(yahooState);
}

int decimalsForSymbol(bool isMarket, const QString& symbol, double value) {
  if (isMarket && !symbol.endsWith(QLatin1String("=X"))) return std::abs(value) >= 1 ? 2 : 4;
  return decimalsFor(std::abs(value));
}

QString symbolPrice(bool isMarket, const QString& symbol, Number value) {
  if (!value) return QString::fromLatin1(kNone);
  return number(value, decimalsForSymbol(isMarket, symbol, *value));
}

QString friendlyName(const QString& symbol) {
  static const QMap<QString, QString> names{{QStringLiteral("^GSPC"), QStringLiteral("S&P 500")}, {QStringLiteral("^IXIC"), QStringLiteral("NASDAQ")},
                                            {QStringLiteral("^FTSE"), QStringLiteral("FTSE 100")}, {QStringLiteral("^DJI"), QStringLiteral("DOW")},
                                            {QStringLiteral("GBPUSD=X"), QStringLiteral("GBP/USD")}, {QStringLiteral("EURUSD=X"), QStringLiteral("EUR/USD")},
                                            {QStringLiteral("GC=F"), QStringLiteral("GOLD")}, {QStringLiteral("CL=F"), QStringLiteral("OIL")}};
  return names.value(symbol, symbol);
}

QString timeframeLabel(const QString& name) {
  static const QRegularExpression minutes(QStringLiteral("^(\\d+)M$"));
  QString label = name;
  return label.replace(minutes, QStringLiteral("\\1m"));
}

}  // namespace rq::format
