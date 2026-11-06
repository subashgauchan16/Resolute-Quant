#include "app/Logging.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTextStream>
#include <mutex>

namespace rq {

namespace {

constexpr qint64 kMaxLogBytes = 1024 * 1024;

QString g_logPath;
std::mutex g_logMutex;

void writeMessage(QtMsgType type, const QMessageLogContext&, const QString& message) {
  if (type == QtDebugMsg || type == QtInfoMsg) return;
  std::lock_guard lock(g_logMutex);
  QFile file(g_logPath);
  const QIODevice::OpenMode mode = file.size() > kMaxLogBytes ? QIODevice::Truncate : QIODevice::Append;
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text | mode)) return;
  const char* level = type == QtWarningMsg ? "warning" : "error";
  QTextStream(&file) << QDateTime::currentDateTime().toString(Qt::ISODate) << ' ' << level << ' ' << message << '\n';
}

}  // namespace

void installFileLogging() {
  const QString folder = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  if (folder.isEmpty() || !QDir().mkpath(folder)) return;
  g_logPath = QDir(folder).filePath(QStringLiteral("native.log"));
  qInstallMessageHandler(writeMessage);
}

}  // namespace rq
