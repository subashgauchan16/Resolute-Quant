"""The single entry point the C++ bridge calls. Arguments and results are JSON text."""

import json

import _rq_host

from . import calculations, health


class JobCancelled(Exception):
    """Raised by a job that saw a cancel request. The bridge reports it as cancelled, not failed."""


class Host:
    """What a job may do while running: report progress and check for a cancel request."""

    def progress(self, fraction, message=""):
        _rq_host.report_progress(float(fraction), str(message))

    def raise_if_cancelled(self):
        if _rq_host.is_cancelled():
            raise JobCancelled("Cancelled")


JOBS = {
    "health": health.check,
    "summary_statistics": calculations.summary_statistics,
    "long_demo": calculations.long_demo,
    "_stuck_for_test": calculations.stuck_for_test,
    "_exit_for_test": calculations.exit_for_test,
}


def run(name, args_json):
    job = JOBS.get(name)
    if job is None:
        raise ValueError(f"Unknown job: {name}")
    return json.dumps(job(json.loads(args_json), Host()), allow_nan=False)
