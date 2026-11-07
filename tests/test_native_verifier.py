"""Use real synthetic ELF programs to check native-result admission."""

import json
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from native_verifier import verify_native


class NativeVerifierContract(unittest.TestCase):
    def setUp(self):
        retained = os.environ.get("OWNER_NATIVE_VERIFIER_OUTPUT")
        if retained:
            self.root = Path(retained) / self._testMethodName
            self.root.mkdir(parents=True, exist_ok=False)
        else:
            self.temp = tempfile.TemporaryDirectory()
            self.addCleanup(self.temp.cleanup)
            self.root = Path(self.temp.name)
        self.tests = self.root / "fixtures"
        fixture = self.tests / "case"
        fixture.mkdir(parents=True)
        (fixture / "native_sim.keymap").write_text(
            "/* Synthetic native fixture marker. */\n"
        )
        (fixture / "native_sim.conf").write_text("CONFIG_OWNER_TEST=y\n")
        (fixture / "events.patterns").write_text("/^EVENT /p\n")
        (fixture / "keycode_events.snapshot").write_text("EVENT 1\n")
        (self.tests / "required_hooks.json").write_text(
            json.dumps([{"configs": ["CONFIG_OWNER_TEST"], "hook": "control"}])
        )
        self.output = self.root / "build"
        binary_dir = self.output / "case" / "zephyr"
        binary_dir.mkdir(parents=True)
        (binary_dir / ".config").write_text("CONFIG_OWNER_TEST=y\n")
        self.binary = binary_dir / "zmk.exe"

    def compile(
        self, *, hook=True, reported=0, process_exit=0, fatal=False, failed_log=False
    ):
        source = self.root / "program.c"
        body = "#include <stdio.h>\n"
        if hook:
            body += (
                "int owner_checked_control(void) { "
                f'puts("OWNER_TEST_RESULT control={reported}"); return 0; }}\n'
            )
        body += "int main(void) { "
        if hook:
            body += "owner_checked_control(); "
        if fatal:
            body += 'puts("ASSERTION FAIL"); '
        if failed_log:
            body += 'puts("<err> owner: FAIL: controlled assertion"); '
        body += f'puts("EVENT 1"); return {process_exit}; }}\n'
        source.write_text(body)
        subprocess.run(
            [
                "cc",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                str(source),
                "-o",
                str(self.binary),
            ],
            check=True,
        )

    def test_positive_repeats_preserve_raw_output(self):
        self.compile()
        verify_native(self.output, self.tests)
        runs = self.binary.parent.parent / "verified-runs"
        self.assertEqual(
            sorted(path.name for path in runs.iterdir() if path.is_dir()),
            ["1", "2", "3"],
        )
        for trial in range(1, 4):
            self.assertEqual(
                (runs / str(trial) / "events.log").read_text(), "EVENT 1\n"
            )
            self.assertIn(
                "OWNER_TEST_RESULT control=0",
                (runs / str(trial) / "raw.log").read_text(),
            )

    def test_missing_checked_init_is_not_snapshot_success(self):
        self.compile(hook=False)
        with self.assertRaisesRegex(AssertionError, "missing checked init"):
            verify_native(self.output, self.tests)

    def test_failed_init_is_not_snapshot_success(self):
        self.compile(reported=-22)
        with self.assertRaisesRegex(AssertionError, "completion"):
            verify_native(self.output, self.tests)

    def test_real_process_exit_is_required(self):
        self.compile(process_exit=7)
        with self.assertRaisesRegex(AssertionError, "native exit 7"):
            verify_native(self.output, self.tests)

    def test_requested_test_cannot_be_disabled_in_build(self):
        self.compile()
        (self.binary.parent / ".config").write_text("# CONFIG_OWNER_TEST is not set\n")
        with self.assertRaisesRegex(AssertionError, "required configuration"):
            verify_native(self.output, self.tests)

    def test_logged_assertion_cannot_pass_with_zero_init_result(self):
        self.compile(failed_log=True)
        with self.assertRaisesRegex(AssertionError, "failed owner assertion"):
            verify_native(self.output, self.tests)

    def test_fixture_requires_its_test_even_when_both_configs_omit_it(self):
        self.compile()
        (self.tests / "required_hooks.json").write_text(
            json.dumps(
                [
                    {
                        "fixtures": ["case"],
                        "configs": ["CONFIG_OWNER_TEST"],
                        "hook": "control",
                    }
                ]
            )
        )
        (self.tests / "case/native_sim.conf").write_text("")
        (self.binary.parent / ".config").write_text("")
        with self.assertRaisesRegex(AssertionError, "required configuration"):
            verify_native(self.output, self.tests)

    def test_fixture_role_cannot_be_replaced_by_a_wrong_built_role(self):
        self.compile()
        (self.tests / "required_hooks.json").write_text(
            json.dumps(
                [
                    {
                        "fixtures": ["case"],
                        "configs": ["CONFIG_OWNER_TEST"],
                        "not_configs": ["CONFIG_ROLE_CENTRAL"],
                        "hook": "control",
                    }
                ]
            )
        )
        (self.binary.parent / ".config").write_text(
            "CONFIG_OWNER_TEST=y\nCONFIG_ROLE_CENTRAL=y\n"
        )
        with self.assertRaisesRegex(AssertionError, "required configuration"):
            verify_native(self.output, self.tests)

    def test_unrelated_fixture_declaration_does_not_require_a_hook(self):
        self.compile(hook=False)
        (self.tests / "required_hooks.json").write_text(
            json.dumps(
                [
                    {
                        "fixtures": ["other"],
                        "configs": ["CONFIG_OWNER_TEST"],
                        "hook": "control",
                    }
                ]
            )
        )
        verify_native(self.output, self.tests)

    def test_missing_native_fixture_cannot_be_a_partial_success(self):
        self.compile()
        other = self.tests / "missing-case"
        other.mkdir()
        (other / "native_sim.keymap").write_text("/* A required second scenario. */\n")
        with self.assertRaisesRegex(AssertionError, "missing native fixtures"):
            verify_native(self.output, self.tests)

    def test_fatal_with_zero_exit_is_rejected(self):
        self.compile(fatal=True)
        with self.assertRaisesRegex(AssertionError, "fatal assertion"):
            verify_native(self.output, self.tests)

    def compile_sanitizer(self, kind):
        source = Path(__file__).with_name("native_sanitizer_fixture.c")
        copied = self.root / source.name
        shutil.copy2(source, copied)
        flags = ["-O1", "-g", "-fno-pie"]
        if kind == "asan":
            flags += [
                "-DSYNTHETIC_ASAN",
                "-fsanitize=address",
                "-fsanitize-recover=address",
            ]
        elif kind == "ubsan":
            flags += [
                "-DSYNTHETIC_UBSAN",
                "-fsanitize=undefined",
                "-fsanitize-recover=undefined",
            ]
        obj = self.root / "control.o"
        commands = [
            [
                "cc",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                *flags,
                "-c",
                str(copied),
                "-o",
                str(obj),
            ],
            ["cc", *flags, "-no-pie", str(obj), "-o", str(self.binary)],
        ]
        (self.root / "compiler-commands.json").write_text(
            json.dumps(commands, indent=2) + "\n"
        )
        for index, command in enumerate(commands):
            result = subprocess.run(command, capture_output=True, text=True)
            (self.root / f"compiler-{index}.log").write_text(
                result.stdout + result.stderr
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        (self.root / "compiled-inputs.json").write_text(
            json.dumps(
                {
                    path.name: {
                        "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                        "bytes": path.stat().st_size,
                        "mode": path.stat().st_mode & 0o7777,
                    }
                    for path in (copied, obj, self.binary)
                },
                indent=2,
            )
            + "\n"
        )

    def recovering_sanitizer_control(self, kind):
        self.compile_sanitizer(kind)
        binary_hash = hashlib.sha256(self.binary.read_bytes()).hexdigest()
        previous = dict(os.environ)
        self.addCleanup(lambda: (os.environ.clear(), os.environ.update(previous)))
        os.environ["ASAN_OPTIONS"] = "halt_on_error=0:detect_leaks=0"
        os.environ["UBSAN_OPTIONS"] = "halt_on_error=0:print_stacktrace=1"
        for trial in range(1, 4):
            with self.subTest(sanitizer=kind, trial=trial):
                output = self.root / f"admission-{trial}"
                shutil.copytree(self.output, output)
                with self.assertRaisesRegex(AssertionError, "sanitizer diagnostic"):
                    verify_native(output, self.tests)
                run = output / "case/verified-runs/1"
                self.assertEqual(
                    json.loads((run / "command.json").read_text())["exit"], 0
                )
                raw = (run / "raw.log").read_text()
                self.assertIn("OWNER_TEST_RESULT control=0\n", raw)
                self.assertIn("EVENT 1\n", raw)
                if kind == "asan":
                    self.assertIn("ERROR: AddressSanitizer: heap-buffer-overflow", raw)
                else:
                    self.assertRegex(
                        raw, r"native_sanitizer_fixture\.c:\d+:\d+: runtime error:"
                    )
                self.assertEqual(
                    hashlib.sha256(
                        (output / "case/zephyr/zmk.exe").read_bytes()
                    ).hexdigest(),
                    binary_hash,
                )
        self.assertEqual(
            hashlib.sha256(self.binary.read_bytes()).hexdigest(), binary_hash
        )

    def test_recovering_asan_zero_exit_and_complete_snapshot_are_rejected(self):
        self.recovering_sanitizer_control("asan")

    def test_recovering_ubsan_zero_exit_and_complete_snapshot_are_rejected(self):
        self.recovering_sanitizer_control("ubsan")

    def test_locationless_runtime_error_label_is_not_a_sanitizer_report(self):
        self.compile_sanitizer("benign")
        verify_native(self.output, self.tests)


if __name__ == "__main__":
    unittest.main(verbosity=2)
