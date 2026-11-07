"""Reports the state of the embedded interpreter."""

import platform
import site
import sys


def check(_args, _host):
    report = {
        "python_version": platform.python_version(),
        "prefix": sys.prefix,
        "path": list(sys.path),
        "isolated": bool(sys.flags.isolated),
        "ignores_environment": bool(sys.flags.ignore_environment),
        "user_site_enabled": bool(site.ENABLE_USER_SITE),
        "numpy_version": None,
        "numpy_error": None,
    }
    try:
        import numpy

        report["numpy_version"] = numpy.__version__
    except ImportError as error:
        report["numpy_error"] = str(error)
    return report
