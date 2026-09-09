# Python automation RED review and implementation plan

Date: 2026-09-09. Status: the Gate A implementation has landed and most of the
RED set is green; N03 and N07 are partly met and the release gates remain open.

This supplements the [backlog](python-api-and-shell.md) and the normative
[execution contract](python-api-contract.md#native-loading-and-automation-contract).
The first feature is native project loading and Python data access. Scripting
slicing, material calculations, estimators, and other operations follows on the
same owned native project representation.

## Review findings and corrections

| Finding | Consequence | Correction |
| --- | --- | --- |
| High: matching the fixed fixture report does not establish native loading | A ZIP/XML reader or fixed response could satisfy counts and fields | Require native importer/ownership review, C++ native geometry comparisons, and native consumer checks below |
| High: no process-level assertion of arbitrary script execution | An option definition or importable module can pass without a working Orca runner | Replaced the option-registration RED with `test_files/run_inspection_red.py`, which launches the supplied Orca executable with two different scripts and a fresh argument token |
| High: retained-child test accessed the mesh before dropping the project and left the source available | Eagerly copied counters or lazy archive reads could conceal missing ownership | First mesh access now occurs after removing a temporary source, releasing the root, and reading another project; mesh must cast to native `TriangleMesh` |
| Medium: fixture script reported per-volume assignment and IDs without checking them | Wrong joins or assignment inheritance could pass | Added unique ID/membership checks, per-part assignment, unused-slot inventory, provenance, and real mesh counts |
| Medium: all real-fixture transforms are identity except translation | Constant scale/rotation implementations could pass | Added native-written parameterized geometry with two copies, volume and instance rotations, nonuniform scale, and mirroring |
| Medium: the 432 mm plate offset had no native read verification; one 0.02 tolerance covered every numeric field | Tests could reward a wrong frame and conceal scale/matrix errors | Removed the unverified plate-local constants; check affine composition and use separate geometry and transform tolerances. Native absolute frame verification remains required |
| Medium: `script.execute` was asserted in a plain embedded-interpreter test | Could encourage unconditional capability advertising | Check that capability in the actual script runner process |
| Medium: documentation described unbuilt Catch2 sources and a manual Python case as a verified normal-target RED set | Overstates executable coverage | Separate added sources, observed runtime failures, planned cases, and required CI integration |

These are findings about the acceptance tests. They were written before the
implementation landed; the sections below record what the implementation now
satisfies.

## Required data flow

```text
OrcaSlicer --file input.3mf --script user.py -- <script arguments>
  -> bundled interpreter executes user.py as __main__
  -> user.py calls orca.host.project.read(path)
  -> native project-loading service uses existing format detection/importers
  -> owned native Model + plate associations + configs + embedded presets
  -> Python read-only project/plate/object/instance/volume views
  -> user code queries, filters, groups, and calculates from those views
  -> later native analysis/slicing services consume the retained native state
```

`--file` supplies the path; the script explicitly loads it with `read()`. This
loads an independent native project without requiring GUI document replacement.
The CLI must run other scripts too; the fixture inspection script has no special
status in production code. JSON is one user script's output format, not the
internal project representation.

## RED-to-GREEN sequence

| ID | Owner | Required test/evidence | State |
| --- | --- | --- | --- |
| RED-N01 | PY-01, PY-03 | Review the call path from `read()` into existing `Model::read_from_archive`/format importers; inspect ownership of `Model`, plates, configs, and presets. A narrow refactor of native loading is allowed. No separate parser, scraped `--info` output, or fixture-specific branches | Met. `ProjectSnapshot::read_file` calls `Model::read_from_archive` for archives and `Model::read_from_file` otherwise, and owns the returned `Model`, `DynamicPrintConfig`, `PlateDataPtrs` and presets. The only native change is in `bbs_3mf.cpp`, which now keeps every `<model_instance>` element and resolves it into `PlateData::objects_and_instances`; nothing new reads ZIP or XML |
| RED-N02 | PY-03, PY-04 | Native writer creates temporary transformed projects of two different sizes with two copies. An independent native read is compared with Python meshes, topology, matrices, and bounds. Mesh must be the existing bound C++ `TriangleMesh` | Met. `Python project geometry matches native imports of generated transformed models` passes for both generated widths |
| RED-N03 | PY-03, PY-04, PY-06 | Compare the real fixture to independently loaded native `PlateData`, model/config/presets, including absolute plate frames. Exercise empty plates, multiple copies of one object, duplicate names, overrides and painting | Partly met. Absolute plate frames are checked against `PartPlateList`'s own origins; empty plates and multiple copies of one object have native fixtures. Duplicate names, per-volume overrides and painting are not covered |
| RED-N04 | PY-02, PY-03 | Drop root, remove only a temporary source, read a second project, then first access retained child geometry. Retain the native mesh after releasing child/second project too | Met. `Python project children retain their owned snapshot` passes |
| RED-N05 | PY-13 | Run two arbitrary user scripts through the real binary on a renamed Unicode fixture path; verify argument token/`__main__`/`__file__`, API capability, graph queries, JSON stdout and unchanged hashes | Met. `run_inspection_red.py` passes against the built executable |
| RED-N06 | PY-02, PY-03, PY-04 | In the C++ harness, obtain the project adapter's retained native state and pass it to an existing native geometry operation after its source disappears. Check imported settings/plate associations too | Met. `Python project state feeds native code after its source is gone` casts the Python `Project` to `host_project::ProjectHandle`, deletes the source, then runs `Model::mesh()` and `bounding_box_exact()` on the retained model and checks the retained config and plate associations. `ProjectHandle` is internal: it exposes no pointer to Python |
| RED-N07 | PY-03, PY-12 | Native import failures, version handling, substitutions, non-mm geometry and nested/external component resources match the native loader policy; metadata-only reports cannot succeed for corrupt required geometry | Partly met. Missing files map to `FileNotFoundError` and a corrupt archive to `ProjectReadError` with no partial report; substitutions become issues. Version handling, non-mm units and external component resources are untested |
| RED-N08 | PY-09, PY-10, PY-11 | A native analysis/slicing job uses a retained project after temporary source removal. Compare geometry estimates/slice results with equivalent direct native input; changing geometry/settings changes results, stale cached statistics do not substitute for computation | Gate B, when the services are implemented |

Behavior checks and architecture checks are both necessary. Even a bound native
mesh can be constructed by an incorrect separate parser. Passing RED-N02/N04/N05
therefore does not waive RED-N01 or RED-N06. A field such as `backend="native"`,
a capability string, a successful `import orca`, or a ban on the Python `zipfile`
module is not evidence of the actual call path. A second C++ XML parser has the
same architectural problem. Existing native importers necessarily read ZIP/XML;
that existing format-layer work is expected and must be reused.

## Commands and completion evidence

From the repository root, using the actual built/packaged Orca executable:

```sh
python3 test_files/run_inspection_red.py --orca-bin /path/to/OrcaSlicer
```

```sh
python3 test_files/run_cli_acceptance.py --orca-bin /path/to/OrcaSlicer
```

Both controllers copy and hash opaque bytes. They never interpret archive XML.
They check successful process exit before accepting JSON; missing runtime/options,
timeouts, malformed output and assertion failures remain failures. They must never
fall back to an external Python 3MF reader. The second one covers CLI-01–CLI-12
and prints one PASS/FAIL/SKIP line per case; a SKIP leaves that case open.

Build the `slic3rutils_tests` target in a test-enabled build (`-DBUILD_TESTS=ON`)
and run `[ProjectInspection]` and `[ScriptCommandLine]`, including generated cases
with randomized Catch2 test order. Follow [tests/AGENTS.md](../../tests/AGENTS.md).
When integrating fixtures into normal suites, follow `tests/data/` conventions and
preserve a documented identity for the user-supplied fixture.

To turn a RED GREEN, record the build/source revision, command, actual failing
assertion before implementation and passing result afterward. Missing API failures
prove only the first unmet boundary; deeper assertions must be observed once it
exists. No skipped required case, hardcoded fixture value in production, mock
native owner, or relaxed assertion without independent evidence can close a gate.

Gate A needs RED-N01–N07 and the existing CLI/inspection DoD. Gate B additionally
needs RED-N08 and its other requirements. Read-only inspection is the first
delivery; no slicing or estimation implementation is required merely to author
these RED tests.

## Verification record, 2026-09-09

Working tree on top of `03369470ad`. Built Release for macOS arm64 with the Xcode
generator into `build/arm64`; the executable under test is
`build/arm64/src/Release/OrcaSlicer.app/Contents/MacOS/OrcaSlicer`.

- `python3 test_files/run_inspection_red.py --orca-bin <executable>` prints
  `PASS: real executable runs both user scripts and preserves the input`.
- `python3 test_files/run_cli_acceptance.py --orca-bin <executable>` reports
  11 passed, 0 failed, 2 skipped. The skips are CLI-08 and CLI-12's
  no-display-server case.
- `OrcaSlicer --file test_files/test_file_01.3mf --script
  test_files/inspect_test_file_01.py -- --verify --compact` exits 0 and prints one
  JSON document. Plate 1's recovered origin is x = 420 mm for the project's
  350 x 320 mm bed, not the withdrawn 432 mm.
- `OrcaSlicer --file test_files/test_file_01.3mf --script
  docs/design/examples/inspect_3mf.py -- --compact` exits 0 with parseable JSON.
- `cmake . -DBUILD_TESTS=ON && cmake --build . --config Release --target
  slic3rutils_tests`, then the suite under `--order rand`: 101 passed, 34 skipped,
  0 failed over 135 cases. `[ProjectInspection]` alone is 9 cases, 412 assertions,
  all passing. The skips are the pre-existing "numpy unavailable in unit-test
  interpreter" and "interpreter is already running" ones.
- The fixture SHA-256 is unchanged at
  `482ec156ee1bf835165fd91c0f2aadf8ed3d695690701c82665ceec0222ba705`.

Two things about this environment are worth recording, because neither is a
property of the code. `catch_discover_tests` cannot list the tests from the built
`slic3rutils_tests.app`: macOS kills the unsigned bundle, and `codesign` refuses
the bundle because of the nested Python tree copied next to the executable. The
suite was therefore run from a copy of the executable and its `python/` directory
outside the bundle, ad-hoc signed. That also means the CTest registration for
these cases is unverified here. Separately, CLI-08 cannot be forced on a developer
build, whose configured bundled-Python fallback root stays reachable however the
executable is relocated.

Gate A's architecture and behavior requirements are met for what is implemented;
A01, A03–A07 and A10–A12 remain open for the reasons recorded in the
[DoD checklist](python-api-contract.md#gate-a-first-cli-inspection-release).
