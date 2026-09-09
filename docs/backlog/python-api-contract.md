# Python API defaults, execution contract, and Definition of Done

Date: 2026-09-08

Status: Gate A is implemented and verified on macOS arm64; the gate itself is not
closed, because several DoD items below still have no evidence. Gate B is
untouched. This document is part of the
[Python API backlog](python-api-and-shell.md).

This document defines delivery scope, default signatures, CLI behavior, and DoD.
The [inspection design](../design/python-project-inspection-api.md) defines the
detailed project schema, coordinates, and material semantics. Changes must keep
both documents, examples, and acceptance cases consistent.

## Delivery boundary

**Gate A: CLI project inspection.** Run a user-supplied Python script inside the
Orca executable, without opening a GUI. Read 3MF files and inspect objects,
instances grouped by plate, volumes, settings, materials, bounds, and transforms.

**Gate B: full initiative.** Add the in-app shell, GUI project opening and plate
reordering, device status snapshots, solid mass estimates, and slicing jobs with
material-consumption estimates. Gate A passing does not mean Gate B is complete.

The first CLI runner initializes file-inspection services only. It does not
attach to a running Orca process or inherit that process's printers or document.
Device services without a GUI and remote process access remain separate follow-up
work. Slicing from a file snapshot can be added at Gate B using native slicing.

## Native loading and automation contract

The feature is an application automation API. `orca.host.project.read()` must
invoke existing Orca native format detection and import logic, retaining an owned
native `Model` plus plate associations, configurations and embedded presets.
Python views reference that storage; native mesh access returns the existing
bound `TriangleMesh`. Extending or refactoring the native importer to retain
missing metadata/provenance is allowed; do not duplicate project interpretation
in Python or in a separate C++ ZIP/XML reader.

Native format code itself reads ZIP/XML as part of loading a 3MF. The prohibited
shortcut is implementing the project API as an archive-metadata report, scraping
CLI output, or recreating native state from JSON/XML-derived Python records.
Fixture names, hashes, counts and expected values belong only in tests. Production
code must load arbitrary supported projects and execute arbitrary user scripts.

After `read()` returns, access must use retained native state without reopening
the source or requiring the original archive. Later analysis/slicing services
take this same state, or a native copy for worker isolation, together with its
settings and plate data. They must not reimport the path or reconstruct a model
from the inspection report. Gate A remains read-only; Gate B adds those operations.

Output equality cannot prove this architecture. Gate A requires source review of
the loading/ownership path and a C++ integration check that the retained project
can be consumed by existing native geometry code. Both are mandatory alongside
behavior tests, as specified in the [RED review and plan](python-api-red-plan.md).

## CLI contract

These forms are equivalent in script mode:

```sh
OrcaSlicer --file assembly.3mf --script inspect.py
OrcaSlicer assembly.3mf --script inspect.py
```

Script arguments follow a delimiter:

```sh
OrcaSlicer --file assembly.3mf --script inspect.py -- --compact
```

### Parsing and defaults

1. `--script <path>` selects a separate process mode that executes exactly one
   script and then exits. It implies operation without a GUI; no `--headless`
   flag is required. Its option order relative to the input path is irrelevant.
2. Accept zero or one model path, provided either by `--file <path>` or as one
   positional argument before `--`. With zero inputs, scripts can supply paths
   themselves through the API. Repeated `--file`, repeated `--script`, multiple
   positional inputs, or combining `--file` and a positional input is an error.
3. In script mode, version one accepts `--script`, `--file`, and `--help` as
   application options, with both `--key value` and `--key=value` forms for paths.
   Reject unknown options and slicing/export/transform options before execution.
   `--file` is valid only with `--script`. `--help` before the delimiter prints
   help and exits successfully without executing a script.
4. The first `--` ends application parsing. Everything after it is forwarded
   unchanged as script arguments, even if it resembles `--script`, `--file`, or
   another Orca option. Detect script mode only from tokens before this delimiter.
5. Without `--script`, preserve the existing CLI and GUI startup behavior,
   including the existing meaning of `--` and positional model arguments.
6. Resolve script and optional model paths against the invocation working
   directory and expose absolute paths without changing the working directory.
   Preserve spaces and Unicode. A path beginning with `-` can use `--file=...`.
7. Validate that the script and any supplied model path are readable regular
   files before running user code. Do not parse/load the 3MF until the script
   calls `project.read()`. A file that disappears later follows normal I/O errors.

### Python execution environment

- Reuse the bundled interpreter and pybind11 module registration. Establish
  resource/data/temp paths before interpreter initialization. Do not start the
  wx event loop, render thumbnails, initialize OpenGL, discover printers, or
  auto-run installed plugins for file inspection. Linking GUI libraries is not
  the same as requiring GUI initialization.
- Execute the file as `__main__`; `__file__` and `sys.argv[0]` are the absolute
  script path. `sys.path[0]` is the script directory so sibling imports work.
  Existing bundled/shared Python package configuration remains available; the
  runner does not install missing packages automatically.
- Set `sys.argv` to `[script_path, *input_paths, *script_args]`, with at most one
  input path. For the third command above this is
  `["/abs/inspect.py", "/abs/assembly.3mf", "--compact"]`. No implicit `project`
  global is injected. The script chooses when to call `read()` and may read more
  than one file itself.
- Establish an explicit user-script execution context. Reuse runtime leases and
  GIL management. Preserve existing plugin audit behavior without treating a
  local user script as a discovered plugin or promising Python sandboxing.
- Stdout belongs to the script, including its imported libraries. Orca startup,
  progress, and diagnostics go to stderr or existing log files; no banners or
  native progress messages may corrupt a script's JSON stdout. Opening a project
  never executes code embedded in that project.
- Flush streams and release jobs/Python objects before interpreter shutdown on
  normal completion and exceptions. Ctrl+C requests cooperative cancellation and
  surfaces `KeyboardInterrupt`; do not forcibly terminate a native worker holding
  locks. Uninterruptible native calls may delay delivery until they return; this
  limitation must be documented and must not be described as immediate cancellation.

### Exit codes

These codes apply only to the new script mode. Existing CLI codes are unchanged.

| Code | Meaning |
| --- | --- |
| `0` | Normal completion, application help, or `SystemExit(None)` |
| `1` | Uncaught Python exception other than the cases below; a noninteger `SystemExit` payload is printed to stderr and returns 1 |
| `2` | Invalid application arguments; the script was not executed |
| `3` | Bundled runtime initialization or `orca` registration failed; the script was not executed |
| `4` | Script/input-file preflight I/O failure, or uncaught `OSError` / `ProjectReadError` (including unsupported 3MF requirements) during execution |
| `130` | Uncaught `KeyboardInterrupt` / cooperative Ctrl+C termination |

An explicit `SystemExit(n)` with integer `0 <= n <= 125` returns `n` unchanged;
these intentional script exits take precedence over automatic error mapping.
Integers outside that range return 1 with a diagnostic instead of platform-specific
truncation. Uncaught exceptions include a traceback on stderr; pre-execution
errors include a concise diagnostic. Caught exceptions do not set the exit code.

## Default namespaces and signatures

`orca.host`, `orca.host.errors` and `orca.host.project`'s file-reading surface are
callable. The rest of the table is still proposed. Add APIs when their
implementation and tests land; do not advertise a placeholder as a completed
capability.

| Namespace | Default purpose | Delivery |
| --- | --- | --- |
| `orca.host` | API version and service capability discovery; preserve existing bindings | A |
| `orca.host.project` | Owned project reads and inspection types | A; GUI snapshots in B |
| `orca.host.errors` | Shared new exception classes; project namespace re-exports its existing proposed error names as aliases | A/B as introduced |
| `orca.host.ops` | Operations on the current GUI document | B |
| `orca.host.devices` | Read-only snapshots from initialized device services | B |
| `orca.host.analysis` | Geometry-based mass estimation | B |
| `orca.host.slicing` | Slicing job submission and result retrieval | B |

The existing `orca.slicing` namespace remains the plugin hook/context API.
`orca.host.slicing` is application job control; adding it does not change plugin
callbacks or permit GUI calls from a slicing hook.

### Version and discovery

```python
# orca.host
api_version: tuple[int, int] = (1, 0)

def capabilities() -> frozenset[str]: ...
```

The version describes the new host contract, independently of the app version.
Additive compatible changes increment the minor version; incompatible changes
require a new major version and a documented migration. Existing raw bindings
keep their current behavior.

Capability names are `script.execute`, `project.read`, `project.snapshot`,
`ops.open_project`, `ops.move_plate`, `devices.status`, `analysis.solid_mass`, and
`slicing.submit`. Return only implemented services usable in this process/session.
Gate A requires `script.execute` and `project.read`. Today `project.read` is
registered when the project bindings load, and `script.execute` only while the CLI
runner is executing a user script, so an embedded interpreter that is not running
one does not advertise it. In the GUI, service availability
can change; every call validates its context even after a capability check.

Once an API is introduced, calling it in an unavailable context raises
`CapabilityUnavailableError`; GUI-dependent calls without a GUI raise its subclass
`ApplicationUnavailableError`. An unavailable service never looks like an empty
successful result. Legacy APIs retain their existing exceptions.

### Project inspection defaults

```python
# orca.host.project; Project and other types follow the inspection design.
def read(path: str | PathLike[str]) -> Project: ...
def snapshot() -> Project: ...  # Gate B; requires GUI

# Project
def material(self, slot_id: int) -> Material: ...

# Instance
def transform(self, *, space: Literal["world", "plate"]) -> Transform: ...
def bounds(self, *, space: Literal["world", "plate"]) -> BoundingBox: ...
def material_assignment(self, volume: Volume) -> MaterialAssignment: ...
def materials(self) -> MaterialSummary: ...

# Plate
def materials(self) -> MaterialSummary: ...

# Volume
def mesh(self) -> TriangleMesh: ...
```

Require the `space` keyword; do not silently choose a coordinate system. Reads
use file data and embedded presets, with no fallback to the GUI's selected
profiles. Normalized fields carry provenance and missing fields remain unknown.
Gate A promises the documented serialized local `config` mappings, not a general
typed/effective-configuration mutation API. Readable core metadata needs no NumPy;
existing mesh-array access may require it.

### Live GUI operations

```python
# orca.host.ops
def open_project(path: str | PathLike[str]) -> Project: ...
def move_plate(project: Project, *, plate_id: str, to_index: int) -> Project: ...
```

`open_project()` follows existing dirty-document/save/cancel handling and returns
an owned snapshot after a successful open. User cancellation raises
`OperationCancelledError`. Loading failure preserves the current document.

`move_plate()` requires a GUI snapshot of the current document/revision. Its
`plate_id` is resolved only within that snapshot. A file snapshot raises
`ValueError`; a replaced or changed GUI document raises `StaleSnapshotError`.
Perform validation and mutation together on the owning thread, so another edit
cannot slip between them. Conflicting background work raises `BusyError`.

`to_index` is the zero-based final position, in `[0, plate_count)`. Reject negative
indexes and invalid IDs with `ValueError`. Reordering preserves the active plate's
identity, contents, relative placement, names, settings, and locks. It creates
one undo entry and returns a new snapshot after updating/invalidation of related
UI and slicing state. Moving to the current position is a no-op with no undo entry
or revision increment. The input snapshot remains unchanged.

### Device status

```python
# orca.host.devices
def list() -> tuple[Machine, ...]: ...
def selected() -> Machine | None: ...
def status(machine_id: str, *, max_age_s: float = 5.0) -> MachineStatus: ...
```

Read existing service state; these calls do not connect, issue printer commands,
or wait for fresh network data. IDs are provider-qualified. An initialized service
with no printers returns `()` / `None`; an unknown ID raises `KeyError`.
`max_age_s` must be finite and nonnegative. Device removal/provider changes during
a read yield a coherent snapshot or an explicit error, not dangling references.

The status contains connection/job state, supported fields, progress in percent,
remaining time in seconds, actual/target temperatures in degrees Celsius where
available, and reported error codes. Unsupported fields are absent from
`supported_fields`; supported but unknown fields have `None` values. Cached data
remains marked stale after disconnect. Age is measured from device observations,
not from the Python read; use the oldest observation among populated telemetry
fields, and mark freshness unknown if their ages cannot be established.

### Solid mass estimation

```python
# orca.host.analysis
def solid_mass(
    project: Project,
    *,
    instance_ids: Sequence[str] | None = None,
    density_g_cm3: float | None = None,
) -> MassEstimate: ...
```

Default scope is every instance with `printable=True`, including unassigned ones.
An explicit ID sequence selects exactly those copies even if nonprintable. Reject
unknown or duplicate IDs. An empty scope returns complete totals of zero.
An explicit density is a uniform override for the entire selected geometry and
must be finite and positive. Otherwise use resolved per-material densities.

Use `mass_g = volume_mm3 * density_g_cm3 / 1000`, including all transforms and
requested copies. Exclude non-part modifiers; handle negative volumes/overlaps
with existing geometry operations or return incomplete with issues. Unknown
density, undecodable material partition, or unsupported geometry yields
`mass_g=None` for the scope. Known partial contributions may be reported separately;
their sum must not masquerade as a complete total. This API never slices a part.

### Slicing jobs and estimates

```python
# orca.host.slicing
def submit(project: Project, *, plate_id: str) -> SliceJob: ...

# SliceJob
def status(self) -> JobStatus: ...
def cancel(self) -> bool: ...
def result(self, *, timeout_s: float | None = None) -> SliceResult: ...
```

Submission accepts an owned file or GUI snapshot and retains it until work/results
are released. Use native configuration resolution/validation; report unavailable
required printer/process/material settings instead of borrowing unrelated GUI
presets. A conflicting job raises `BusyError` in the first implementation.
No upload, physical print start, or automatic source-file export occurs.

States are `queued`, `running`, `succeeded`, `failed`, and `cancelled`; terminal
states never transition again. `cancel()` returns True only when it newly accepts
a cancellation request before a terminal state. Request acceptance is not immediate
completion. A cancellation/completion race ends in one consistent terminal state.

`result()` returns only a succeeded result. It raises `SlicingError` for failure,
`OperationCancelledError` for cancellation, or `TimeoutError` if the wait expires.
`timeout_s=None` waits indefinitely; `0` polls; finite nonnegative values bound the
wait. Release the GIL during waits. On the GUI thread a pending job raises
`BusyError` instead of blocking the UI; completed results remain readable there.

Results identify their retained input snapshot and plate. Later project edits
cannot alter them, and a result from an older GUI revision is historical, not
current. Imported cached statistics cannot be returned as a freshly computed
result. Expose total consumption separately from finished-part/support/purge
breakdowns. Unavailable category or per-object accounting stays `None`; do not
estimate it by assigning the plate total to each object.

## Data structure concepts and invariants

| Type | Contract |
| --- | --- |
| `Project` | Owns model/config/plate data; `plates`, `objects`, `instances`, `materials`, `unassigned_instances`, `issues` are immutable tuples; `metadata`/`config` are read-only mappings. Also carries `source_kind`, `source_path`, `source_document_id: str | None`, `source_revision: int | None`. Last two are both None for a file read. |
| `Plate` | Opaque ID, zero-based order index, name, optional lock state and plate-to-world transform, local config, assigned instances. A missing frame does not remove the plate. |
| `Object` / `Volume` | An object definition can have multiple copies and volumes. Volumes preserve role, local geometry, object-space transform, and overrides. Meshes share only immutable storage. |
| `Instance` | One copy of an object, unique within the snapshot, with exactly one plate ID or None. Membership is independent of its printable toggle. |
| `Transform` / `BoundingBox` | Millimeters; transforms use column vectors and `T @ Rz @ Ry @ Rx @ scale @ mirror`. Degree-valued convenience rotation; full affine matrix remains authoritative. Bounds are axis-aligned positive-part envelopes. Empty bounds are explicitly undefined. |
| `Material` | One-based filament slot ID, optional name/type/color/vendor/preset/density/diameter, kind and mixed-slot component IDs, per-field provenance. A slot is not an AMS bay or physical nozzle. |
| `MaterialAssignment` / `MaterialSummary` | Default vs painted references, resolution source, and completeness. Summaries deduplicate by slot ID and report uncertainty; they are reference inventories, not consumed material. |
| `Machine` | `id: str`, `provider: str`, `name: str`, `capabilities: frozenset[str]`; no credentials. Device IDs are separate from project snapshot IDs. |
| `MachineStatus` | `machine_id: str`, `connection_state: str`, `job_state: str | None`, `supported_fields: frozenset[str]`, `progress_percent: float | None`, `remaining_time_s: float | None`, `temperatures: tuple[Temperature, ...]`, `error_codes: tuple[str, ...]`, `observed_at_utc: datetime | None`, `age_s: float | None`, `stale: bool`. |
| `Temperature` | `component_id: str`, `actual_c: float | None`, `target_c: float | None`; use explicit bed/chamber/nozzle identity rather than assuming a single nozzle. |
| `MassEstimate` | `method="solid_geometry"`, `instance_ids: tuple[str, ...]`, `volume_mm3: float | None`, `mass_g: float | None`, `complete: bool`, `per_material: tuple[MaterialMass, ...]`, `issues: tuple[Issue, ...]`. |
| `MaterialMass` | `slot_id: int | None`, `volume_mm3: float | None`, `mass_g: float | None`, `density_g_cm3: float | None`, `density_source` is `override`, `material`, or `unavailable`; explicit uniform overrides can yield a contribution with no known slot ID. |
| `JobStatus` | `state: str`, `progress: float | None` in `[0,1]`, `message: str | None`; values are copied when polled. |
| `SliceResult` | Retained `project: Project`, `plate_id: str`, `statistics: PrintEstimate`; immutable historical result. |
| `PrintEstimate` | `total_mass_g`, `model_mass_g`, `support_mass_g`, `purge_mass_g`, `unclassified_mass_g`, `filament_length_mm`, `extruded_volume_mm3`, `estimated_time_s`: each `float | None`; `per_material: tuple[MaterialUsage, ...]`, `issues: tuple[Issue, ...]`. |
| `MaterialUsage` | `slot_id: int`, `mass_g: float | None`, `length_mm: float | None`, `volume_mm3: float | None`; slot IDs follow the retained project snapshot. |
| `Issue` | Stable machine-readable `code`, explanatory `message`, optional snapshot `entity_id`. Scripts branch on codes; messages may evolve. |

Snapshot IDs are stable only within their owning snapshot; sequences use import
order except documented material sorting. Child wrappers retain storage after
their parent variable is released. Default `read()` never writes its source file,
changes the active project, or persists profile/config changes. Native scratch
files and explicitly requested script output are separate from the source file.

GUI `source_document_id` changes on document replacement. `source_revision` is a
monotonically increasing mutation token: edits, undo, redo, relevant preset
changes, and plate operations advance it; undo does not reuse an earlier token.
Validate both tokens before applying an operation. A returned snapshot may have
new snapshot-local entity IDs, so callers must use the returned graph afterwards.

For populated telemetry, `stale` is True when disconnected, age is unknown, or
`age_s > max_age_s`. Normalize connection states to `connected`, `connecting`,
`disconnected`, or `unknown`, and job states to `idle`, `preparing`, `printing`,
`paused`, `finished`, `failed`, or `unknown` when supported; unsupported job state
is None. Unknown provider values produce `unknown` rather than a guessed state.

Every numeric result is finite or None; invalid data produces issues/errors.
Mass categories are mutually exclusive. When all categories are available,
their sum including unclassified material matches total mass within the published
tolerance; otherwise missing categories stay None. A geometry estimate is never
called the sliced/printed mass. No automatic slicing is hidden inside inspection.

Shared errors in `orca.host.errors`: `CapabilityUnavailableError`, its subclass
`ApplicationUnavailableError`, `ProjectReadError`, its subclass
`UnsupportedProjectError`, `BusyError`, `StaleSnapshotError`,
`OperationCancelledError`, and `SlicingError` derive from `RuntimeError` unless
their parent is named above. `CoordinateUnavailableError` derives from
`ValueError`. The classes for the operations that exist are registered;
`BusyError`, `StaleSnapshotError`, `OperationCancelledError` and `SlicingError`
arrive with the Gate B services that raise them. Retain standard `TypeError`, `ValueError`, `KeyError`, `OSError`, and
`TimeoutError` where specified. Re-export matching project error names as aliases
to the same class objects, not a second hierarchy.

## CLI acceptance cases

These are required executable-level tests for PY-13, not passing tests today.
The probe script prints `sys.argv`, `__name__`, `__file__`, working directory, and
sibling-import output as one JSON object. Other scripts deliberately raise the
listed exception or write stdout/stderr. Author them as test fixtures during
implementation, using the existing test setup.

| ID | Scenario | Required observation |
| --- | --- | --- |
| CLI-01 | Named input and equivalent positional input | Identical probe payloads after normalizing invocation paths; exit 0; exactly one script execution |
| CLI-02 | Script with no input and arguments after `--` | No invented model argument; tail tokens unchanged, including option-looking strings |
| CLI-03 | Repeated/missing option values, two inputs, mixed named/positional input, incompatible `--slice` | Exit 2; probe marker absent; no GUI or model mutation |
| CLI-04 | `--help` before delimiter; `--help` after delimiter | First prints application help without running script; second reaches the script |
| CLI-05 | Spaces/Unicode in script, input, working directory, and sibling module paths | Successful import/read; exact argument strings and documented path resolution |
| CLI-06 | Real inspection script reads a multi-plate fixture | Stdout is valid JSON with correct object/instance/material joins, units, and transforms; source hash unchanged |
| CLI-07 | Missing script/input; corrupt 3MF passed to `read()` | Exit 4 and stderr diagnostic/traceback; no successful partial report |
| CLI-08 | Missing bundled interpreter or failed module initialization | Exit 3; user script not executed; no GUI fallback |
| CLI-09 | Script normal return, uncaught `ValueError`, `OSError`, and explicit `SystemExit(7)` | Codes 0, 1, 4, 7 respectively; caught errors permit normal success |
| CLI-10 | Script writes both streams while native code logs | Script stdout stays exact; diagnostics on stderr/log; streams flushed on exit |
| CLI-11 | Ctrl+C during a long Python loop and a cooperatively cancellable native job | Exit 130, no deadlock or orphan worker; native interruption latency is documented |
| CLI-12 | No display server on Linux; normal invocations without `--script` on all platforms | Inspection works without X/Wayland; existing help/info/slice/export and GUI launch behavior remains compatible |

At Gate A, CLI-11's Python-loop case is mandatory. Its native slicing-job case is
mandatory at Gate B when that service lands. Tests of executable failures use
isolated temporary runtime layouts, not edits to the developer's installation.

## Definition of Done

| Contract area | Backlog owner | Required gate evidence |
| --- | --- | --- |
| Version, capabilities, exceptions | PY-01 | A02, A11 |
| Ownership, threads, shutdown | PY-02 | A07, B02, B06 |
| CLI parsing/execution | PY-13 | A01 and CLI-01–CLI-12 |
| File reads and GUI opening | PY-03 | A03, A04, A08, A12; GUI portion B03 |
| Model/material/coordinate inspection | PY-04, PY-06 | A03–A08 |
| Interactive shell | PY-05 | B02 |
| Plate reordering | PY-07 | B03 |
| Device status | PY-08 | B04 |
| Solid mass | PY-09 | B05 |
| Slice jobs and consumption | PY-10, PY-11 | B06, B07 |
| Documentation and compatibility | PY-12 | A09–A11, B08 |

### Active test set

The C++ cases are registered in the `slic3rutils` suite and pass. The process
controllers under `test_files/` are manual and still need CTest/CI registration.
Every `[RED]` tag has been removed: none of these express unmet behavior any more.

| Test | State |
| --- | --- |
| `Python host advertises project inspection only when it is available` | Passes |
| `Python project namespace exposes the immutable inspection graph` | Passes |
| `Python project reader loads a Unicode 3MF without a GUI` | Passes |
| `Python project children retain their owned snapshot` | Passes |
| `Python project geometry matches native imports of generated transformed models` | Passes, both generated widths |
| `Python project reader maps a missing file to FileNotFoundError` | Passes |
| `Python project keeps every copy on exactly one plate` | Passes; covers two copies of one object, an empty plate and an unassigned copy |
| `Python plate frames match the application's own plate layout` | Passes against `PartPlateList`'s own origins |
| `Python project state feeds native code after its source is gone` | Passes; RED-N06 |
| `test_files/inspect_test_file_01.py --verify` through the real executable | Passes; exit 0 |
| `test_files/run_inspection_red.py --orca-bin <binary>` | Passes |
| `test_files/run_cli_acceptance.py --orca-bin <binary>` | 11 pass, 2 skip (CLI-08, and CLI-12b off Linux) |

The twelve draft cases in
[project_inspection_contract.py](../design/examples/project_inspection_contract.py)
have still never run: their fixtures are unauthored, and their plate-origin
expectations need correcting first. That keeps A03 open.

### Gate A: first CLI inspection release

Recorded against the working tree on top of `03369470ad`, built Release for macOS
arm64 and run as
`build/arm64/src/Release/OrcaSlicer.app/Contents/MacOS/OrcaSlicer`.

- [ ] A01 — CLI-01 through CLI-12 pass against the real executable, with the
  documented Gate A exception for a not-yet-implemented native slicing job.
  `run_cli_acceptance.py` reports 11 passed, 0 failed, 2 skipped on macOS. Open on
  CLI-08 (no isolated runtime layout: a developer build still resolves its
  configured bundled-Python fallback, so the runtime cannot be made missing) and on
  CLI-12's no-display-server case, which only runs on Linux.
- [x] A02 — Version/capability discovery matches the implemented runtime.
  `api_version` is `(1, 0)`; `capabilities()` returns `project.read` in an embedded
  interpreter and adds `script.execute` only inside the CLI runner, observed in
  both the Catch2 harness and `run_inspection_red.py`. No GUI or device service is
  advertised, because none is introduced yet.
- [ ] A03 — All 12 [draft inspection cases](../design/examples/project_inspection_contract.py)
  are exercised by the real bindings using authored fixtures, integrated into
  the repository's tests. Open: the fixtures are unauthored and the file's
  plate-origin expectations contradict the native grid layout.
- [ ] A04 — Representative Orca, Bambu, Prusa, and generic 3MF files are read using
  native format/version/configuration logic. Covered: a real Orca/Bambu project, a
  generic 3MF with a Unicode path and no plate metadata, and Prusa-written archives
  produced by `store_3mf` in the geometry-parity case. Open: non-mm units, and
  explicit malformed/unsupported-version cases beyond the corrupt-archive check in
  `run_cli_acceptance.py`.
- [ ] A05 — Additional geometry/membership checks cover nonidentity volume
  transforms, non-90-degree rotations, empty geometry/plates, duplicate names,
  unassigned copies, missing plate frames, and conflicting/broken membership.
  Covered: nonidentity volume transforms and non-90-degree rotations (generated
  parity case), empty plates, unassigned copies, and two copies of one object on
  one plate. Open: empty geometry, duplicate names, missing plate frames, and
  conflicting/broken membership, which the reader reports but no test exercises.
- [ ] A06 — Material checks cover inherited/overridden/painted assignments,
  unused and sparse slots, mixed slots, missing metadata/density, provenance, and
  unsupported normalization. Covered: object-inherited assignment, unused slots,
  and per-field provenance on the real fixture. Open: volume overrides, painting,
  mixed slots, missing density, and embedded-preset fallback.
- [ ] A07 — Scalar geometry/material reads work without NumPy. When NumPy is
  available, shared mesh arrays are read-only and remain valid for their promised
  lifetime. The acceptance scripts read bounds, transforms, materials and mesh
  counts with no NumPy import, and the lifetime case holds a mesh past its
  project's release. Open: no test pins the read-only array behaviour or concurrent
  reads for project snapshots specifically.
- [x] A08 — The [inspection example](../design/examples/inspect_3mf.py) works as a
  CLI script with documented arguments and produces parseable JSON. Run against the
  real fixture: exit 0, one JSON document, and the source SHA-256 unchanged.
- [x] A08a — `test_files/inspect_test_file_01.py --verify` exits 0 against
  `test_files/test_file_01.3mf`. Its JSON reports two plates, three objects, three
  instances, one instance on plate 0, two on plate 1, all five configured material
  slots, and slot 1 assigned to every model part. Plate 1's frame is x = 420 mm and
  agrees with `PartPlateList`'s own origin for the same bed in
  `Python plate frames match the application's own plate layout`. The process
  controller ran both scripts; the fixture SHA-256 is unchanged.
- [x] A09 — Existing plugin/binding tests and relevant core import tests pass;
  normal startup and existing CLI commands retain their prior behavior. The full
  `slic3rutils` suite is 101 passed, 34 skipped, 0 failed under `--order rand`; the
  skips are the pre-existing "numpy unavailable" and "interpreter already running"
  ones. CLI-12 confirms `--help` and `--info` are unchanged.
- [ ] A10 — The executable builds and the applicable CLI/binding tests pass on
  Windows, macOS, and Linux. Only macOS arm64 has been run.
- [ ] A11 — Actual docstrings and generated stubs match the shipped signatures,
  exceptions, units, ordering, and lifetime rules. Every class, method and property
  of `orca.host.project` carries a docstring with its type, units and lifetime
  rules, and [docs/python_api](../python_api/README.md) renders them into a Sphinx
  site generated from the running application, built with warnings as errors so a
  dead cross-reference fails. Open: the `.pyi` stubs have not been regenerated, and
  nothing compares docstrings against this contract automatically.
- [ ] A12 — RED-N01–N07 satisfy the native-loading contract. N01, N02, N04, N05 and
  N06 are met; N03 and N07 are partly met. See the
  [RED review and plan](python-api-red-plan.md).

### Gate B: complete initiative

- [ ] B01 — Gate A remains green, and every original item PY-01 through PY-13
  meets its own DoD, including the GUI portion of PY-03 and full PY-12 verification.
- [ ] B02 — The shell supports persistent state, multiline/Unicode input,
  history, output/errors, and reset without restarting the shared interpreter.
  Responsiveness, close/reopen, plugin coexistence, and shutdown are verified.
- [ ] B03 — Opening/reordering a GUI project validates snapshot origin/revision,
  preserves document state on errors/cancel, maintains selection and plate data,
  and performs one undoable change. Test no-op, stale snapshot, busy state,
  undo/redo, and save/reopen through existing GUI export.
- [ ] B04 — Device tests distinguish missing service, zero devices, no selection,
  unsupported fields, unknown values, stale/disconnected data, and agent changes.
  Simulated status checks pass; supported fields are compared with the device UI
  on available providers, with the exact verified provider coverage recorded.
- [ ] B05 — Solid mass tests prove dimensions/density/transforms/copy counts with
  analytic fixtures, including incomplete material/geometry cases and explicit
  density overrides. Totals cannot silently include unsupported partial results.
- [ ] B06 — Slicing tests cover correct configuration, success/failure,
  cancellation, finite/zero timeout, UI-thread result access, project edits during
  work, and runtime shutdown. Immutable inputs/results retain correct provenance.
  RED-N08 verifies native consumption after temporary source removal without
  reimporting or reconstructing a project from the Python inspection report.
- [ ] B07 — Print estimates agree with native/GUI totals within explicit test
  tolerances for walls, infill, supports, purge, and multiple materials. Historical
  or cached statistics never appear as fresh results for changed settings.
- [ ] B08 — All newly added GUI/runtime flows are built and checked on Windows,
  macOS, and Linux. Existing projects/profiles retain compatibility; any required
  format migration is implemented and verified. No change to slicing results or
  GUI defaults occurs merely because scripting support is present.

### Evidence required to close an item or gate

Record the source revision/build configuration, commands, exit codes, test names,
fixture versions, and results. For manual checks, record platform/provider and
the observed scenario/result. Use explicit numeric tolerances based on fixture
dimensions/settings and native precision; do not bless arbitrary discrepancies.
Keep evidence in the implementing PR or a linked verification record.

A defect, skipped required case, or unavailable required platform run keeps its
DoD entry open. A documented limitation satisfies a case only where this contract
explicitly permits unavailable/incomplete data or delayed cancellation. API names,
examples, mock output, compilation alone, and the existence of this checklist do
not establish runtime correctness.
