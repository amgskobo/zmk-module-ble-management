import platform
import os
import tempfile
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

from dataclasses import dataclass

from native_verifier import verify_native
from test_native_verifier import NativeVerifierContract as NativeVerifierContract

from test_build_budget import BuildBudgetContract as BuildBudgetContract

THIS_DIR = Path(__file__).resolve().parent.parent


def run_west(args: list[str]) -> subprocess.CompletedProcess[str]:
    if args and args[0] == "zmk-build":
        parallelism = int(os.environ.get("OWNER_TEST_BUILD_PARALLELISM", "1"))
        if parallelism < 1:
            raise ValueError("OWNER_TEST_BUILD_PARALLELISM must be a positive integer")
        args = [args[0], "--parallelism", str(parallelism), *args[1:]]
    return subprocess.run(
        ["west", *args],
        capture_output=True,
        text=True,
        cwd=THIS_DIR,
    )


@dataclass
class NotFound:
    text: str


class WestCommandsTests(unittest.TestCase):
    WEST_TOPDIR: Path
    BUILD_DIR: Path

    @unittest.skipUnless(
        shutil.which("cc"), "split-info host contract requires a C compiler"
    )
    def test_split_info_payload(self):
        result = subprocess.run(
            [sys.executable, "tests/split-info/run.py"],
            cwd=THIS_DIR,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    @classmethod
    def setUpClass(cls):
        cls.WEST_TOPDIR = Path(
            subprocess.run(
                ["west", "topdir"],
                capture_output=True,
                text=True,
            ).stdout.strip()
        )
        requested_output = os.environ.get("OWNER_TEST_OUTPUT")
        if requested_output:
            cls.BUILD_DIR = Path(requested_output).resolve()
            cls.BUILD_DIR.mkdir(parents=True, exist_ok=False)
        else:
            cls.BUILD_DIR = Path(tempfile.mkdtemp(prefix=THIS_DIR.name + "-tests-"))
        print(f"Owner test output: {cls.BUILD_DIR}", flush=True)

    @unittest.skipUnless(
        platform.system() == "Linux", "zmk-test is only supported on Linux"
    )
    def test_zmk_test(self):
        tests_build = self.BUILD_DIR / "tests"

        result = run_west(["zmk-test", "tests", "-m", ".", "-d", str(tests_build)])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        verify_native(tests_build, THIS_DIR / "tests")
        self.assertIn("PASS: studio", result.stdout)

    def test_zmk_build(self):
        artifacts_and_expected_config: dict[str, list[str | NotFound]] = {
            "my_awesome_keyboard_with_custom_rpc_support": [
                "CONFIG_MY_AWESOME_KEYBOARD_SPECIAL_FEATURE=y",
                "CONFIG_ZMK_STUDIO=y",
                "CONFIG_ZMK_BLE_MANAGEMENT=y",
                "CONFIG_ZMK_BLE_MANAGEMENT_STUDIO_RPC=y",
            ],
            "my_awesome_keyboard_without_custom_rpc_support": [
                "CONFIG_MY_AWESOME_KEYBOARD_SPECIAL_FEATURE=y",
                "# CONFIG_ZMK_STUDIO is not set",
                "CONFIG_ZMK_BLE_MANAGEMENT=y",
                NotFound("CONFIG_ZMK_BLE_MANAGEMENT_STUDIO_RPC"),
            ],
        }

        result = run_west(
            [
                "zmk-build",
                "tests/zmk-config/config",
                "-m",
                "tests/zmk-config",
                ".",
                "-q",
                "-P",
                "1",
                "-d",
                str(self.BUILD_DIR),
            ]
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        for artifact, entries in artifacts_and_expected_config.items():
            config_path = self.BUILD_DIR / artifact / "zephyr" / ".config"
            self.assertTrue(config_path.exists(), f"{artifact} .config is missing")
            config_text = config_path.read_text()
            for entry in entries:
                if isinstance(entry, NotFound):
                    if entry.text in config_text:
                        self.fail(
                            f"{entry.text} found in {config_path} for {artifact}, but it should not be present"
                        )
                else:
                    if entry not in config_text:
                        self.fail(f"{entry} not found in {config_path} for {artifact}")
            self.assertTrue(
                (config_path.parent / "zmk.uf2").exists(),
                f"{artifact} zmk.uf2 is missing in {config_path.parent}",
            )


if __name__ == "__main__":
    unittest.main()
