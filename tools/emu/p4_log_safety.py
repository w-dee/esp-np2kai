#!/usr/bin/env python3
"""Shared fatal-log signatures for P4 host-side evidence validators."""

from __future__ import annotations

import re


FATAL_PATTERNS = (
    re.compile(r"Guru Meditation Error", re.IGNORECASE),
    re.compile(r"panic'ed", re.IGNORECASE),
    re.compile(r"^\s*panic\s*:", re.IGNORECASE | re.MULTILINE),
    re.compile(r"assert failed", re.IGNORECASE),
    re.compile(r"abort\(\) was called", re.IGNORECASE),
    re.compile(r"Stack (?:overflow|smashing)", re.IGNORECASE),
    re.compile(r"(?:Task watchdog got triggered|Interrupt wdt timeout)",
               re.IGNORECASE),
    re.compile(r"ESP_ERROR_CHECK failed", re.IGNORECASE),
    re.compile(r"unhandled (?:fatal )?exception", re.IGNORECASE),
)


def fatal_pattern_names(text: str) -> tuple[str, ...]:
    """Return the shared fatal patterns found in decoded UART text."""
    return tuple(pattern.pattern for pattern in FATAL_PATTERNS if pattern.search(text))
