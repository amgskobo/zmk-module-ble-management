#!/usr/bin/env python3
"""Replay retained real sanitizer ELF controls through a selected verifier."""

import argparse
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import os
import shutil


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verifier", type=Path, required=True)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--expect", choices=("accepted", "rejected"), required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    spec = importlib.util.spec_from_file_location(
        "selected_native_verifier", args.verifier
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    os.environ["ASAN_OPTIONS"] = "halt_on_error=0:detect_leaks=0"
    os.environ["UBSAN_OPTIONS"] = "halt_on_error=0:print_stacktrace=1"
    records = []
    for kind in ("asan", "ubsan"):
        fixture = (
            args.fixtures
            / f"test_recovering_{kind}_zero_exit_and_complete_snapshot_are_rejected"
        )
        retained = fixture / "build/case/zephyr/zmk.exe"
        digest = hashlib.sha256(retained.read_bytes()).hexdigest()
        for trial in range(1, 4):
            output = args.output / f"{kind}-{trial}"
            shutil.copytree(fixture / "build", output)
            captured = io.StringIO()
            error = None
            with contextlib.redirect_stdout(captured):
                try:
                    module.verify_native(output, fixture / "fixtures")
                except AssertionError as exception:
                    error = str(exception)
            (output / "verifier.log").write_text(
                captured.getvalue() + (error or "") + "\n"
            )
            runs = sorted((output / "case/verified-runs").glob("[1-3]"))
            assert len(runs) == (3 if args.expect == "accepted" else 1), runs
            for run in runs:
                command = json.loads((run / "command.json").read_text())
                raw = (run / "raw.log").read_text()
                assert command["exit"] == 0, command
                assert raw.count("OWNER_TEST_RESULT control=0\n") == 1, raw
                assert raw.count("EVENT 1\n") == 1, raw
                expected = (
                    "ERROR: AddressSanitizer: heap-buffer-overflow"
                    if kind == "asan"
                    else "runtime error: signed integer overflow"
                )
                assert expected in raw, raw
                if args.expect == "accepted":
                    assert not (run / "snapshot.diff").read_text()
            assert (
                hashlib.sha256(
                    (output / "case/zephyr/zmk.exe").read_bytes()
                ).hexdigest()
                == digest
            )
            if args.expect == "rejected":
                assert error and "sanitizer diagnostic" in error, error
            else:
                assert error is None, error
            records.append(
                {
                    "sanitizer": kind,
                    "trial": trial,
                    "binary_sha256": digest,
                    "native_processes": len(runs),
                    "native_exits": [0] * len(runs),
                    "admission": args.expect,
                    "verifier_error": error,
                }
            )
            (args.output / "results.json").write_text(
                json.dumps(records, indent=2) + "\n"
            )
    print(
        json.dumps(
            {
                "expected_admissions": len(records),
                "status": "PASS",
                "expect": args.expect,
            }
        )
    )


if __name__ == "__main__":
    main()
