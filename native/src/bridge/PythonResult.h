#pragma once

#include <QJsonObject>
#include <QMetaType>
#include <QString>

namespace rq {

enum class JobStatus { Ok, Error, Cancelled };

// A finished job, in plain C++ and Qt types. No Python types reach the window.
struct JobResult {
  quint64 id = 0;
  QString job;
  JobStatus status = JobStatus::Error;
  QJsonObject value;   // The job's result, when status is Ok.
  QString errorType;   // For example "ValueError", when status is Error.
  QString message;
  QString traceback;
};

}  // namespace rq

Q_DECLARE_METATYPE(rq::JobResult)
