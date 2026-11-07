#pragma once

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QVector>
#include <QWidget>
#include <optional>

class QTimer;

namespace rq {

// A panel title bar: amber label on the left, small grey note on the right.
class PanelHead : public QWidget {
  Q_OBJECT

 public:
  PanelHead(QString title, QString note, QWidget* parent = nullptr);
  void setNote(const QString& note);
  QString note() const { return note_; }

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  QString title_;
  QString note_;
};

// The watchlist: market rows then crypto rows, each with a sparkline, price and change.
class WatchlistView : public QWidget {
  Q_OBJECT

 public:
  struct Row {
    QString symbol;
    bool market = false;
    QString title;
    QString tag;
    QString price = QStringLiteral("-");
    QString change = QStringLiteral("-");
    int changeSign = 0;
    QVector<double> spark;
    int flash = 0;
    QElapsedTimer flashClock;
  };

  explicit WatchlistView(QWidget* parent = nullptr);

  void setSymbols(const QStringList& markets, const QStringList& crypto);
  void updateRow(const QString& symbol, const QString& price, const QString& tag, const QString& change, int changeSign, const QVector<double>& spark, int flash);
  void setSelected(const QString& symbol);
  void setNote(const QString& note);
  void setDimmed(bool crypto, bool markets);

  const Row* row(const QString& symbol) const;
  QPoint rowCentre(const QString& symbol) const;
  QSize sizeHint() const override;

 signals:
  void symbolClicked(const QString& symbol);

 protected:
  void paintEvent(QPaintEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void leaveEvent(QEvent* event) override;

 private:
  int rowAt(int y) const;
  int yOfRow(int index) const;
  int noteHeight() const;
  void paintGroup(QPainter& painter, int y, const QString& left, const QString& right);
  void paintRow(QPainter& painter, int y, Row& row);

  QVector<Row> rows_;
  int marketCount_ = 0;
  QString selected_;
  QString note_;
  int hovered_ = -1;
  bool dimCrypto_ = false;
  bool dimMarkets_ = false;
  QTimer* flashTimer_;
};

// The order book: ten asks, the spread, then ten bids, with cumulative depth bars.
class OrderBookView : public QWidget {
  Q_OBJECT

 public:
  explicit OrderBookView(QWidget* parent = nullptr);
  // bids and asks hold [price, size] pairs, best first. Empty clears the book.
  void setBook(const QJsonArray& bids, const QJsonArray& asks);
  void clearBook();
  void setDimmed(bool dimmed);
  QString spreadText() const { return spread_; }
  QString midText() const { return mid_; }
  std::optional<double> imbalance() const { return imbalance_; }
  QSize sizeHint() const override;

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  struct Level {
    double price;
    double size;
    double cumulative;
  };

  void paintLevels(QPainter& painter, int top, const QVector<Level>& levels, bool asks, double maximum);

  QVector<Level> bids_;
  QVector<Level> asks_;
  QString spread_ = QStringLiteral("-");
  QString mid_ = QStringLiteral("-");
  std::optional<double> imbalance_;
  bool dimmed_ = false;
};

// Time and sales: newest first. Large trades are marked.
class TapeView : public QWidget {
  Q_OBJECT

 public:
  explicit TapeView(QWidget* parent = nullptr);
  void setTrades(const QJsonArray& trades);
  int rowCount() const { return static_cast<int>(trades_.size()); }

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  QJsonArray trades_;
};

// The stats row along the bottom: seven labelled figures.
class StatsStrip : public QWidget {
  Q_OBJECT

 public:
  struct Cell {
    QString label;
    QString value = QStringLiteral("-");
    QColor colour;  // Invalid means the default text colour.
    int valueSize = 17;
    QString note;
    std::optional<double> fill;    // A green bar, 0 to 1.
    std::optional<double> marker;  // An amber tick, 0 to 1.
    bool hasBar = false;
  };

  explicit StatsStrip(QWidget* parent = nullptr);
  void setCells(const QVector<Cell>& cells);
  void setDimmed(bool dimmed);
  const QVector<Cell>& cells() const { return cells_; }

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  QVector<Cell> cells_;
  bool dimmed_ = false;
};

}  // namespace rq
