"""The single entry point the C++ bridge calls. Arguments and results are JSON text."""

import json

import _rq_host

from . import calculations, health, live, market_history, storage


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
    "history_options": market_history.options,
    "history": market_history.load,
    "history_export": market_history.export_csv,
    "terminal_start": live.start,
    "terminal_stop": live.shutdown,
    "terminal_select": live.select,
    "terminal_watch": live.watch,
    "terminal_resolve": live.resolve,
    "live_chart": live.chart,
    "market_chart": live.market_chart,
    "market_profile": live.market_profile,
    "market_news": live.market_news,
    "storage_status": storage.status_job,
    "_storage_self_test_for_test": storage.self_test_job,
    "_terminal_start_fixture_for_test": live.start_fixture_for_test,
    "_live_drop_fixture_for_test": live.drop_fixture_for_test,
    "_live_rest_calls_for_test": live.fixture_rest_calls_for_test,
    "_history_fixture_for_test": market_history.fixture_for_test,
    "_import_market_for_test": market_history.import_for_test,
    "_stuck_for_test": calculations.stuck_for_test,
    "_exit_for_test": calculations.exit_for_test,
}


def run(name, args_json):
    job = JOBS.get(name)
    if job is None:
        raise ValueError(f"Unknown job: {name}")
    return json.dumps(job(json.loads(args_json), Host()), allow_nan=False)


def shutdown():
    """Called by the bridge just before the interpreter closes."""
    live.shutdown()
