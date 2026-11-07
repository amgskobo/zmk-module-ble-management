"""Recognize sanitizer reports without treating ordinary error labels as faults."""

import re


SANITIZER_REPORT = re.compile(
    r"(?:\b(?:ERROR|WARNING|SUMMARY): "
    r"(?:Address|Leak|Memory|Thread|UndefinedBehavior)Sanitizer:"
    r"|\bAddressSanitizer:DEADLYSIGNAL"
    r"|^[^\n]*?:\d+(?::\d+)?: runtime error:)",
    re.M,
)


def has_sanitizer_report(output):
    return SANITIZER_REPORT.search(output) is not None
