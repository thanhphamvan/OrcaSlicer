"""Process-level RED: invoke the real Orca executable with two user scripts.

python3 test_files/run_inspection_red.py --orca-bin /path/to/OrcaSlicer

Uses only the standard library. This controller copies/hashes opaque fixture
bytes; all project interpretation happens inside Orca via orca.host.project.
"""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import uuid

from inspect_test_file_01 import verify_test_file_01


QUERY_SCRIPT = '''import json
import sys
import orca

project = orca.host.project.read(sys.argv[1])
print(json.dumps({
    "nonce": sys.argv[2],
    "name": __name__,
    "file": __file__,
    "argv": sys.argv,
    "capabilities": sorted(orca.host.capabilities()),
    "plate_model_names": [sorted(i.object.name for i in p.instances) for p in project.plates],
    "used_slot_ids": sorted({m.slot_id for p in project.plates for m in p.materials().items}),
    "configured_slot_count": len(project.materials),
}))
'''


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run_script(executable, cwd, arguments):
    process = subprocess.run(
        [str(executable), *arguments], cwd=cwd, capture_output=True,
        text=True, encoding="utf-8", timeout=45,
    )
    if process.returncode != 0:
        raise AssertionError(
            f"Orca script invocation returned {process.returncode}; expected 0.\n"
            f"stderr:\n{process.stderr}\nstdout:\n{process.stdout}"
        )
    # Reject banners, multiple documents, trailing non-JSON text, and NaN/Infinity.
    def invalid_constant(value):
        raise AssertionError(f"Invalid JSON number: {value}")
    return json.loads(process.stdout, parse_constant=invalid_constant)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--orca-bin", type=Path, required=True)
    args = parser.parse_args()
    if not __debug__:
        parser.error("RED verification requires Python assertions enabled")
    executable = args.orca_bin.resolve(strict=True)
    fixture_dir = Path(__file__).resolve().parent
    fixture = fixture_dir / "test_file_01.3mf"
    original_digest = digest(fixture)
    try:
        with tempfile.TemporaryDirectory(prefix="orca inspection ü ") as directory:
            work = Path(directory)
            source = work / "renamed project ü.3mf"
            inspector = work / "user inventory.py"
            query = work / "user query.py"
            shutil.copyfile(fixture, source)
            shutil.copyfile(fixture_dir / "inspect_test_file_01.py", inspector)
            query.write_text(QUERY_SCRIPT, encoding="utf-8")
            report = run_script(executable, work, [
                "--file", source.name, "--script", inspector.name, "--", "--verify", "--compact",
            ])
            verify_test_file_01(report)
            assert Path(report["source_path"]) == source.resolve()
            assert digest(source) == original_digest

            nonce = uuid.uuid4().hex
            result = run_script(executable, work, [
                source.name, "--script", query.name, "--", nonce,
            ])
            assert result["nonce"] == nonce
            assert result["name"] == "__main__"
            assert result["file"] == str(query.resolve())
            assert result["argv"] == [str(query.resolve()), str(source.resolve()), nonce]
            assert {"script.execute", "project.read"} <= set(result["capabilities"])
            assert result["plate_model_names"] == [
                ["3DBenchy.drc"], ["OrcaCube_v2.drc", "OrcaPlug_v2.drc"],
            ]
            assert result["used_slot_ids"] == [1]
            assert result["configured_slot_count"] == 5
            assert digest(source) == original_digest
    finally:
        assert digest(fixture) == original_digest, "Original fixture changed"
    print("PASS: real executable runs both user scripts and preserves the input")


if __name__ == "__main__":
    main()
