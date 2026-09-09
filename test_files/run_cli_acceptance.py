"""Executable acceptance cases CLI-01 .. CLI-12 for OrcaSlicer's --script mode.

    python3 test_files/run_cli_acceptance.py --orca-bin /path/to/OrcaSlicer

Standard library only. The controller never interprets a 3MF: it copies opaque
bytes and hashes them, and every project fact comes from Orca itself. Cases that
cannot run on the current platform are reported as SKIP with the reason, and a
skipped case leaves its DoD entry open rather than counting as a pass.

See docs/backlog/python-api-contract.md for the normative contract.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import signal
import subprocess
import sys
import tempfile
import time

TIMEOUT = 120

# Prints the execution context as one JSON object, plus the marker the
# "script must not run" cases look for.
PROBE = '''import json, os, sys
import sibling
print(json.dumps({
    "marker": "probe-ran",
    "argv": sys.argv,
    "name": __name__,
    "file": __file__,
    "cwd": os.getcwd(),
    "sys_path0": sys.path[0],
    "sibling": sibling.greeting(),
    "capabilities": sorted(__import__("orca").host.capabilities()),
    "api_version": list(__import__("orca").host.api_version),
}))
'''

SIBLING = 'def greeting():\n    return "sibling-import-ok"\n'

# One script covering the exit-code table; the mode arrives after the delimiter.
EXIT_CODES = '''import sys
mode = sys.argv[-1]
if mode == "ok":
    print("done")
elif mode == "value":
    raise ValueError("deliberate failure")
elif mode == "oserror":
    open("/definitely/not/here/at/all.txt", "rb")
elif mode == "read-missing":
    import orca
    orca.host.project.read("/definitely/not/here/at/all.3mf")
elif mode == "read-corrupt":
    import orca
    orca.host.project.read(sys.argv[1])
elif mode == "exit7":
    sys.exit(7)
elif mode == "exit-none":
    sys.exit(None)
elif mode == "caught":
    try:
        raise ValueError("handled")
    except ValueError:
        pass
    print("done")
else:
    raise SystemExit("unknown mode " + mode)
'''

STREAMS = '''import sys
sys.stderr.write("script-stderr-line\\n")
print("script-stdout-line")
sys.stderr.write("script-stderr-line-2\\n")
'''

INTERRUPT = '''import sys, time
print("started", flush=True)
while True:
    time.sleep(0.05)
'''


class Failure(AssertionError):
    pass


class Harness:
    def __init__(self, executable: Path, work: Path, fixture: Path):
        self.executable = executable
        self.work = work
        self.fixture = fixture
        self.results: list[tuple[str, str, str]] = []

    # -- process helpers ---------------------------------------------------
    def run(self, arguments, cwd=None):
        return subprocess.run(
            [str(self.executable), *arguments], cwd=str(cwd or self.work),
            capture_output=True, text=True, encoding="utf-8", timeout=TIMEOUT,
        )

    def case(self, identifier, description, body):
        try:
            body()
        except Failure as error:
            self.results.append((identifier, "FAIL", f"{description}: {error}"))
        except Exception as error:  # noqa: BLE001 - reported, not swallowed
            self.results.append((identifier, "FAIL", f"{description}: {error!r}"))
        else:
            self.results.append((identifier, "PASS", description))

    def skip(self, identifier, description, reason):
        self.results.append((identifier, "SKIP", f"{description}: {reason}"))

    # -- assertions --------------------------------------------------------
    @staticmethod
    def expect(condition, message):
        if not condition:
            raise Failure(message)

    def expect_exit(self, process, code):
        self.expect(
            process.returncode == code,
            f"expected exit {code}, got {process.returncode}\n"
            f"stdout:\n{process.stdout}\nstderr:\n{process.stderr}",
        )

    def probe_payload(self, process):
        self.expect_exit(process, 0)
        try:
            return json.loads(process.stdout)
        except json.JSONDecodeError as error:
            raise Failure(f"stdout is not one JSON document ({error}):\n{process.stdout}") from None


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build_cases(harness: Harness) -> None:
    work = harness.work
    fixture = harness.fixture
    probe = work / "probe.py"
    probe.write_text(PROBE, encoding="utf-8")
    (work / "sibling.py").write_text(SIBLING, encoding="utf-8")
    exit_codes = work / "exit_codes.py"
    exit_codes.write_text(EXIT_CODES, encoding="utf-8")
    streams = work / "streams.py"
    streams.write_text(STREAMS, encoding="utf-8")
    interrupt = work / "interrupt.py"
    interrupt.write_text(INTERRUPT, encoding="utf-8")
    model = work / "model.3mf"
    shutil.copyfile(fixture, model)
    original = digest(model)

    def cli01():
        named = harness.probe_payload(harness.run(["--file", "model.3mf", "--script", "probe.py"]))
        positional = harness.probe_payload(harness.run(["model.3mf", "--script", "probe.py"]))
        harness.expect(named["argv"] == positional["argv"], "named and positional argv differ")
        harness.expect(named["argv"] == [str(probe.resolve()), str(model.resolve())],
                       f"unexpected argv {named['argv']}")
        harness.expect(named["marker"] == "probe-ran", "the script did not run")
        harness.expect(named["name"] == "__main__", f"__name__ is {named['name']}")

    def cli02():
        payload = harness.probe_payload(
            harness.run(["--script", "probe.py", "--", "--file", "-x", "--script"]))
        harness.expect(payload["argv"] == [str(probe.resolve()), "--file", "-x", "--script"],
                       f"tail tokens were altered: {payload['argv']}")

    def cli03():
        invalid = [
            ["--script", "probe.py", "--script", "probe.py"],
            ["--script", "probe.py", "--file", "model.3mf", "--file", "model.3mf"],
            ["--script", "probe.py", "--file", "model.3mf", "model.3mf"],
            ["--script", "probe.py", "model.3mf", "model.3mf"],
            ["--script", "probe.py", "--slice", "0"],
            ["--script", "probe.py", "--export-3mf", "out.3mf"],
            ["--script"],
        ]
        for arguments in invalid:
            process = harness.run(arguments)
            harness.expect(process.returncode == 2,
                           f"{arguments} exited {process.returncode}, expected 2\n{process.stderr}")
            harness.expect("probe-ran" not in process.stdout, f"{arguments} executed the script")
        harness.expect(digest(model) == original, "an invalid invocation modified the model")

    def cli04():
        before = harness.run(["--script", "probe.py", "--help"])
        harness.expect_exit(before, 0)
        harness.expect("probe-ran" not in before.stdout, "--help before the delimiter ran the script")
        harness.expect("--script" in before.stdout, f"help text missing:\n{before.stdout}")

        after = harness.probe_payload(harness.run(["--script", "probe.py", "--", "--help"]))
        harness.expect(after["argv"][-1] == "--help", "--help after the delimiter did not reach the script")

    def cli05():
        unicode_dir = work / "Geräte ü 中文"
        unicode_dir.mkdir(exist_ok=True)
        unicode_script = unicode_dir / "user inventory ü.py"
        unicode_script.write_text(PROBE, encoding="utf-8")
        (unicode_dir / "sibling.py").write_text(SIBLING, encoding="utf-8")
        unicode_model = unicode_dir / "renamed project ü.3mf"
        shutil.copyfile(fixture, unicode_model)

        payload = harness.probe_payload(harness.run(
            ["--file", unicode_model.name, "--script", unicode_script.name], cwd=unicode_dir))
        harness.expect(payload["sibling"] == "sibling-import-ok", "sibling import failed")
        harness.expect(payload["argv"] == [str(unicode_script.resolve()), str(unicode_model.resolve())],
                       f"unexpected argv {payload['argv']}")
        harness.expect(Path(payload["sys_path0"]) == unicode_script.resolve().parent,
                       f"sys.path[0] is {payload['sys_path0']}")
        harness.expect(Path(payload["cwd"]) == unicode_dir.resolve(),
                       f"working directory changed to {payload['cwd']}")
        harness.expect(payload["file"] == str(unicode_script.resolve()), "__file__ is not the absolute script path")

        # A path starting with '-' is reachable through the --file=... form.
        dashed = unicode_dir / "-leading dash.3mf"
        shutil.copyfile(fixture, dashed)
        dashed_payload = harness.probe_payload(harness.run(
            [f"--file={dashed.name}", "--script", unicode_script.name], cwd=unicode_dir))
        harness.expect(dashed_payload["argv"][1] == str(dashed.resolve()), "dashed path was not resolved")

    def cli06():
        inspector = work / "inspect_test_file_01.py"
        shutil.copyfile(Path(__file__).resolve().parent / "inspect_test_file_01.py", inspector)
        process = harness.run(["--file", "model.3mf", "--script", inspector.name, "--", "--verify", "--compact"])
        harness.expect_exit(process, 0)
        report = json.loads(process.stdout)
        harness.expect(report["plate_count"] == 2, "expected two plates")
        harness.expect(digest(model) == original, "reading the project modified it")

    def cli07():
        missing_script = harness.run(["--script", "no-such-script.py"])
        harness.expect_exit(missing_script, 4)
        harness.expect(missing_script.stderr.strip() != "", "no diagnostic for a missing script")

        missing_input = harness.run(["--script", "probe.py", "--file", "no-such-model.3mf"])
        harness.expect_exit(missing_input, 4)
        harness.expect("probe-ran" not in missing_input.stdout, "the script ran despite a missing input")

        corrupt = work / "corrupt.3mf"
        corrupt.write_bytes(b"PK\x03\x04 this is not a real archive")
        process = harness.run(["--script", exit_codes.name, corrupt.name, "--", "read-corrupt"])
        harness.expect_exit(process, 4)
        harness.expect(process.stdout.strip() == "", f"a failed read still produced stdout: {process.stdout}")

    def cli09():
        for mode, expected in (("ok", 0), ("caught", 0), ("exit-none", 0), ("value", 1),
                               ("oserror", 4), ("read-missing", 4), ("exit7", 7)):
            process = harness.run(["--script", exit_codes.name, "--", mode])
            harness.expect(process.returncode == expected,
                           f"mode {mode} exited {process.returncode}, expected {expected}\n{process.stderr}")
            if expected in (1, 4):
                harness.expect("Traceback" in process.stderr, f"mode {mode} printed no traceback")

    def cli10():
        process = harness.run(["--script", streams.name])
        harness.expect_exit(process, 0)
        harness.expect(process.stdout == "script-stdout-line\n",
                       f"native output leaked into stdout:\n{process.stdout}")
        harness.expect("script-stderr-line" in process.stderr, "the script's stderr was lost")
        harness.expect("script-stderr-line-2" in process.stderr, "stderr was not flushed on exit")

    def cli11():
        process = subprocess.Popen(
            [str(harness.executable), "--script", interrupt.name], cwd=str(work),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8")
        try:
            deadline = time.monotonic() + TIMEOUT
            while time.monotonic() < deadline:
                line = process.stdout.readline()
                if line.strip() == "started":
                    break
            else:
                raise Failure("the script never started")
            process.send_signal(signal.SIGINT)
            try:
                _, stderr = process.communicate(timeout=60)
            except subprocess.TimeoutExpired:
                raise Failure("the process did not exit after Ctrl+C") from None
            harness.expect(process.returncode == 130,
                           f"expected exit 130, got {process.returncode}\nstderr:\n{stderr}")
            harness.expect("KeyboardInterrupt" in stderr, "no KeyboardInterrupt traceback")
        finally:
            if process.poll() is None:
                process.kill()

    def cli12():
        # Existing behaviour must be unchanged when --script is absent.
        help_process = harness.run(["--help"])
        harness.expect(help_process.returncode == 0, f"--help exited {help_process.returncode}")
        harness.expect("--slice" in help_process.stdout or "--slice" in help_process.stderr,
                       "the existing CLI help no longer lists its own options")
        info = harness.run(["--info", "model.3mf"])
        harness.expect(info.returncode == 0, f"--info exited {info.returncode}\n{info.stderr}")
        harness.expect("size_x" in info.stdout or "size_x" in info.stderr,
                       f"--info produced no model information:\n{info.stdout}\n{info.stderr}")
        harness.expect(digest(model) == original, "--info modified the model")

    harness.case("CLI-01", "named and positional input are equivalent", cli01)
    harness.case("CLI-02", "no input, tail tokens forwarded verbatim", cli02)
    harness.case("CLI-03", "invalid application arguments exit 2", cli03)
    harness.case("CLI-04", "--help before and after the delimiter", cli04)
    harness.case("CLI-05", "spaces and Unicode in every path", cli05)
    harness.case("CLI-06", "real inspection script over a multi-plate fixture", cli06)
    harness.case("CLI-07", "missing script/input and a corrupt project", cli07)
    harness.skip("CLI-08", "missing bundled runtime exits 3",
                 "needs an isolated runtime layout; not implemented by this controller")
    harness.case("CLI-09", "exit codes for return, ValueError, OSError and SystemExit(7)", cli09)
    harness.case("CLI-10", "the script owns stdout while native code logs", cli10)
    if platform.system() == "Windows":
        harness.skip("CLI-11", "Ctrl+C during a Python loop exits 130",
                     "SIGINT delivery differs on Windows; run this case there separately")
    else:
        harness.case("CLI-11", "Ctrl+C during a Python loop exits 130", cli11)
    harness.case("CLI-12", "invocations without --script keep their behaviour", cli12)
    if platform.system() != "Linux":
        harness.skip("CLI-12b", "inspection without a display server",
                     f"only meaningful on Linux, running on {platform.system()}")
    else:
        def cli12b():
            environment = dict(os.environ)
            environment.pop("DISPLAY", None)
            environment.pop("WAYLAND_DISPLAY", None)
            process = subprocess.run(
                [str(harness.executable), "--file", "model.3mf", "--script", "probe.py"],
                cwd=str(work), capture_output=True, text=True, encoding="utf-8",
                timeout=TIMEOUT, env=environment)
            harness.expect_exit(process, 0)
            harness.expect("probe-ran" in process.stdout, "inspection failed without a display server")

        harness.case("CLI-12b", "inspection without a display server", cli12b)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--orca-bin", type=Path, required=True)
    arguments = parser.parse_args()
    if not __debug__:
        parser.error("acceptance verification requires Python assertions enabled")

    executable = arguments.orca_bin.resolve(strict=True)
    fixture = Path(__file__).resolve().parent / "test_file_01.3mf"
    fixture_digest = digest(fixture)

    with tempfile.TemporaryDirectory(prefix="orca cli acceptance ") as directory:
        harness = Harness(executable, Path(directory), fixture)
        build_cases(harness)

    if digest(fixture) != fixture_digest:
        print("FAIL: the checked-in fixture was modified", file=sys.stderr)
        return 1

    failures = 0
    for identifier, status, description in harness.results:
        print(f"{status:4} {identifier}  {description}")
        failures += status == "FAIL"
    skipped = sum(status == "SKIP" for _, status, _ in harness.results)
    print(f"\n{len(harness.results) - failures - skipped} passed, {failures} failed, {skipped} skipped")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
