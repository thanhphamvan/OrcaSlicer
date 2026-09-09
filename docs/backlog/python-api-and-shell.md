# Python API, CLI scripting, and interactive shell backlog

Date: 2026-09-08

Status: Milestone 2 is implemented — a CLI script runs inside Orca and reads a
project through owned native state — and verified on macOS. Milestones 3 and 4 have
not started, and Gate A is not closed: see its
[DoD checklist](python-api-contract.md#gate-a-first-cli-inspection-release) for the
items that still have no evidence.

The [default APIs, execution contract, and Definition of Done](python-api-contract.md)
are the implementation baseline for this backlog. All delivery checkboxes remain
open until verified against real bindings and fixtures. Writing a design or
syntax-checking an example does not complete its implementation item.

Related design: [Python project inspection API](../design/python-project-inspection-api.md)
defines the proposed namespace, data model, examples, and acceptance cases for
PY-01–PY-04 and PY-06.

The [RED review and plan](python-api-red-plan.md) maps the required native-loading,
ownership, executable and future automation tests to these items. Passing a
fixture report alone does not satisfy the implementation DoD.

## Objective

Provide a Python API usable first from a CLI script runner, then from an
interactive shell inside OrcaSlicer. Users should be able to read a 3MF project,
inspect its structure and settings, query connected printers, reorder plates,
and estimate model mass before printing.

Extend the existing embedded `orca` module and plugin runtime. Keep existing
Python plugins, slicing behavior, project files, and printer profiles compatible.
First deliver Orca executing user Python that loads and queries a native project.
Later slicing and estimation operations consume the same retained native data.
An independent ZIP/XML metadata reader does not meet this objective; see the
[native-loading contract](python-api-contract.md#native-loading-and-automation-contract).

## Existing foundation

These findings describe the inspected checkout, not a released API guarantee.

| Area | Existing implementation | Gap for this work |
| --- | --- | --- |
| Python runtime | [PythonInterpreter](../../src/slic3r/plugin/PythonInterpreter.hpp), pybind11, and the embedded [orca module](../../src/slic3r/plugin/PythonPluginBridge.cpp) | Interactive execution and a documented application API contract |
| Application access | [PluginHostApp](../../src/slic3r/plugin/host/PluginHostApp.cpp) exposes `orca.host.model()`, `plater()`, and `preset_bundle()` | Access requires an initialized GUI; project operations are not bound |
| Model structure | [PluginHostModel](../../src/slic3r/plugin/host/PluginHostModel.cpp) exposes objects, volumes, instances, transforms, metadata, and configuration reads | Complete project traversal and persistent-reference behavior |
| Meshes and settings | [PluginHostMesh](../../src/slic3r/plugin/host/PluginHostMesh.cpp) exposes immutable mesh snapshots and NumPy arrays; [PluginHostPresets](../../src/slic3r/plugin/host/PluginHostPresets.cpp) exposes presets and serialized configuration values | Document units and setting precedence; fill inspection gaps |
| 3MF loading | [Model::read_from_archive](../../src/libslic3r/Model.cpp), [3mf](../../src/libslic3r/Format/3mf.hpp), and [bbs_3mf](../../src/libslic3r/Format/bbs_3mf.hpp) load models, settings, plate data, and embedded presets | Python project read/open operations with explicit ownership |
| Plates | [PartPlateList](../../src/slic3r/GUI/PartPlate.hpp) has plate access and `move_plate_to_index()`; [Plater](../../src/slic3r/GUI/Plater.cpp) coordinates GUI updates and undo | Python plate inspection and a complete reorder operation |
| Printer state | [DeviceManager](../../src/slic3r/GUI/DeviceCore/DevManager.h) and [MachineObject](../../src/slic3r/GUI/DeviceManager.hpp) maintain connected-device state | A host API to read status; implementing a Python printer agent is a separate capability |
| Slicing and mass | [PluginHostSlicing](../../src/slic3r/plugin/host/PluginHostSlicing.cpp) exposes slicing graphs during hooks; [PrintStatistics](../../src/libslic3r/Print.hpp) and [GCode](../../src/libslic3r/GCode.cpp) calculate usage and weight | Starting a slice from Python and retrieving valid, scoped estimates |
| Developer tooling | [Stub generator](../../scripts/generate_orca_python_stubs.py) and [binding tests](../../tests/slic3rutils/test_plugin_host_api.cpp) exist | Coverage and examples for the new surface |

The document graph is `Model -> ModelObject -> ModelVolume / ModelInstance`.
Volumes hold geometry; instances place objects in the scene. Plate membership,
project settings, and embedded presets require additional project data. The
current model binding alone is not a complete representation of a 3MF project.

## Default scope and decisions

- Start with `OrcaSlicer --file project.3mf --script inspect.py`, using the bundled
  interpreter without starting a GUI. Preserve positional model input as an
  alternative. The in-app shell follows and shares this runtime/API.
- Reuse `orca.host` and existing bindings. Default new signatures and behavior
  are specified in [the contract](python-api-contract.md); implementation changes
  to that contract must update the examples and acceptance cases in the same PR.
- Distinguish reading a file into an owned project representation from opening it
  in the live GUI. Reading a file must not replace the user's current project.
- Interpret “reorder plates” as changing plate order while retaining their
  contents. Arranging objects on a plate is a separate follow-up feature.
- Initially expose printer status as read-only snapshots. Supported fields depend
  on the active connection provider and device capabilities. The first CLI
  release does not initialize device services or attach to an existing GUI.
- Cover both solid geometry mass and estimates derived from slicing. Never label
  total filament consumption as the mass of the finished part.
- Prefer existing application operations and helpers. Any new shared abstraction
  must address a concrete gap, especially ownership or thread coordination.

## Delivery order

P0 establishes the runtime and CLI foundation. P1 delivers the remaining requested
workflows. Milestone 2 is implemented; the rest is open. IDs remain stable even
where delivery order differs from numerical order.

| Milestone | Items | Exit condition |
| --- | --- | --- |
| 1. Runtime contracts | PY-01, PY-02 | Python access has defined ownership, threading, and compatibility behavior — implemented |
| 2. CLI inspection | PY-13, file-reading part of PY-03, PY-04, PY-06, relevant PY-12 checks | Gate A: a CLI script reads a 3MF and produces a correct plate/material/geometry report — implemented, gate not closed |
| 3. Live application operations | PY-05, GUI-opening part of PY-03, PY-07, PY-08 | A user can inspect/reorder live plates and read machine status |
| 4. Mass estimates and full release verification | PY-09, PY-10, PY-11, remaining PY-12 checks | Gate B: all original workflows meet the full DoD |

Gate A does not close PY-03's GUI-opening work or the full initiative. See the
[DoD checklists and evidence requirements](python-api-contract.md#definition-of-done).

## Milestone 1: Runtime contracts

### PY-01 — Define the public API contract

Priority: P0. Dependencies: none.

- Implement and document the version/capability contract, live application access,
  owned project data, snapshots, and operations.
  Specify units, index conventions, identifier scope, configuration value types,
  and unavailable-value handling. Account for existing 1-based filament/extruder
  identifiers rather than silently changing them.
- Define errors for invalid arguments, unavailable GUI/device state, stale
  references, busy operations, and cancellation. Define how scripts discover
  supported API capabilities and how additive changes remain compatible.
- Record the shell's execution context within the existing plugin audit/runtime
  system, including how it differs from plugin callbacks. Preserve current plugin
  permissions and avoid presenting arbitrary Python execution as a sandbox.

DoD: the [contract](python-api-contract.md) and inspection design agree; actual
bindings advertise only available capabilities; version/error/argument tests pass;
existing public binding behavior remains supported. A draft schema alone is not
completion.

### PY-02 — Make persistent access safe across application changes

Priority: P0. Dependencies: PY-01.

- Audit non-owning model, instance, volume, preset, plate, and slicing references.
  Define which values are snapshots and which become invalid after document
  replacement, deletion, undo/redo, or completion of a slicing hook.
- Reuse runtime leases, GIL handling, and existing UI dispatch patterns where
  applicable. Keep GUI-owned data on its owning thread and heavy work off the UI
  thread. Do not introduce synchronous dispatch cycles between slicing and UI
  threads; the existing slicing-hook restrictions still apply.
- Make stale access fail predictably or retain an explicitly owned snapshot.
  Clean up shell callbacks and jobs during interpreter/application shutdown.

DoD: retained Python values cannot dereference deleted C++ objects; project
replacement and undo/redo behave as documented; shell work and slicing can coexist
without deadlocks. Add targeted lifetime and thread-coordination verification.
RED-N04 and RED-N06 require native child/mesh access after releasing the root,
removing a temporary source, and loading a second project.

## Milestone 2: CLI inspection

### PY-13 — Run Python scripts from the CLI

Priority: P0. Dependencies: PY-01, PY-02; integrated inspection requires PY-03,
PY-04, and PY-06.

- Add `--script` execution and its `--file`/positional-input handling before the
  existing GUI/slicing dispatch. Split script arguments after `--` before handing
  application arguments to the existing option parser.
- Initialize the bundled runtime and bindings with no GUI, display server,
  printer discovery, or automatic plugin execution. Honor the exact `sys.argv`,
  path, stdout/stderr, exception, exit-code, and shutdown contract.
- Run the supplied Python file as `__main__`, with its directory available for
  imports. File reading remains explicit through `orca.host.project.read()`.
- Preserve existing commands when `--script` is absent. Script mode rejects
  conflicting slicing/export/transform actions instead of executing both.

DoD: every [CLI acceptance case](python-api-contract.md#cli-acceptance-cases)
passes through the real executable, including Unicode paths, script arguments,
Ctrl+C, JSON output isolation, runtime failure, and operation without a display.
Gate A additionally requires the real 3MF inspection contract cases.
RED-N05 runs two user scripts through the real executable; option registration
and embedded-module imports alone do not close this item. Integrate the manual
process controller into CTest/CI as part of implementation.

### PY-03 — Read and open 3MF projects

Priority: P1. Dependencies: PY-01, PY-02.

- Expose file inspection with ownership of the loaded model, plate metadata,
  configuration, and embedded presets. Reuse native format detection, version
  checks, and configuration substitution logic.
- Expose opening a project in the GUI through the existing application workflow,
  preserving dirty-project handling and cancellation. Document whether results
  represent an independent project or the current live document.
- Report invalid archives, unsupported versions, missing data, and substitutions
  through the API. Never execute Python merely because a 3MF was opened.

DoD: inspect representative Orca, Bambu, Prusa, and generic 3MF fixtures;
unavailable extension data is explicit. Independent reads leave the live project
unchanged; failed loads do not partially replace it. Include Unicode paths.
Track file reading for Gate A and GUI opening for Gate B separately; GUI opening
uses `orca.host.ops.open_project()` and returns a snapshot of the opened document.
RED-N01 and RED-N06 require review of the native importer and owned native
project adapter plus a native consumer test. A separate ZIP/XML reader, even if
written in C++ or matching the fixture output, fails this DoD. RED-N07 covers
native import compatibility and errors.

### PY-04 — Complete model and configuration inspection

Priority: P1. Dependencies: the file-reading portion of PY-03.

- Inventory and reuse existing bindings for objects, volumes, instances, names,
  IDs, mesh arrays, transforms, bounds, materials, and modifier roles.
- Expose missing project metadata and setting scopes needed for inspection:
  printer, process, filament, plate, object, and volume. `config` reads local
  serialized values; normalized material fields follow the documented resolution
  and provenance rules. A general effective-setting API remains follow-up scope.
- Document local/object/world coordinates, transform composition, millimeter
  units, instance multiplicity, and the immutability of shared mesh arrays.

DoD: an example enumerates a multi-object, multi-instance project and
explains where its settings originate. Results match the native model and GUI;
absent settings are distinguishable from valid zero/empty values.
RED-N02 compares native meshes, topology, matrices and bounds on generated
transformed inputs; RED-N03 compares material resolution with native reference
data. JSON is produced by the example, not used as the internal project model.

The first real-file acceptance is `test_files/test_file_01.3mf`, exercised by
`test_files/inspect_test_file_01.py --verify`. It must expose three objects and
three instances across two plates, including both stored world placement and
native plate-local placement, identity rotation/scale, bounds, five configured
material slots, and slot 1 assignments for all three parts.

### PY-06 — Expose plate inventory and membership

Priority: P1. Dependencies: the file-reading portion of PY-03 and PY-04.

- Expose plate order, names, identity, active selection, lock state, plate settings,
  and object/instance membership for loaded projects and the live application.
- Represent empty plates and unassigned/unprintable instances explicitly. Treat
  plate index as position, with documented identity behavior across reordering.
- Preserve empty plates, imported order, missing plate frames, and unresolved
  membership exactly as specified by the inspection contract. Availability and
  validity of slicing statistics are completed with PY-10/PY-11.

DoD: Python inventory matches the GUI and imported plate metadata for
empty, single-plate, multi-plate, and multi-instance projects.
For `test_file_01.3mf`, the ordered plate instance counts are exactly `[1, 2]`
with no unassigned instances.
RED-N03 requires an independent native plate frame and membership reference;
composition of two mutually consistent but incorrect matrices is insufficient.

## Milestone 3: Live application operations

### PY-05 — Add an interactive Python shell

Priority: P1. Dependencies: PY-02, PY-04, PY-13.

- Add a discoverable shell window or panel with a persistent namespace, multiline
  input, command history, expression results, stdout/stderr, and tracebacks.
  Scripts and plugins use the same application API.
- Support completion/help from the exposed API without triggering arbitrary
  application mutations during completion. Provide a way to clear output and
  reset the shell namespace without restarting the shared interpreter.
- Keep the UI responsive, bound displayed output, and define interruption
  behavior. Long native operations use cooperative cancellation; document where
  immediate interruption is unavailable. Preserve existing Python error logging.

DoD: interactive inspection works while the GUI remains usable; a Python
exception does not terminate the app; repeated open/close and namespace reset do
not unload plugins or leak callbacks. Verify Unicode input and multiline paste.

### PY-07 — Reorder plates through an application operation

Priority: P1. Dependencies: PY-02, PY-06.

- Wrap the existing move behavior with index validation, one undo snapshot, and
  the associated selection, preview, object-list, and slicing-context updates.
  Reuse the GUI operation path; avoid duplicating its side effects in bindings.
- Preserve plate contents, relative object placement, names, settings, and locks.
  Define active-plate behavior and reject or coordinate work while slicing or
  another conflicting operation is active.
- Treat moving a plate to its current position as a documented no-op. Invalid
  requests leave the project unchanged.

DoD: move first/middle/last plates, undo and redo, then save through the
existing GUI and reopen the 3MF. Order and membership survive the round trip;
selection and cached slicing results remain consistent or are invalidated. Stale
GUI snapshots and snapshots loaded from a file cannot mutate the live project.

### PY-08 — Query machine status

Priority: P1. Dependencies: PY-01, PY-02.

- Expose known machines and the selected machine through the active device
  manager, with device identity and provider/capability information.
- Return copied status values such as connection state, print state, progress,
  temperatures, remaining time, and reported errors where supported. Distinguish
  unsupported, unknown, stale, and disconnected state; document freshness.
- Read existing provider data and authoritative `MachineObject` fields. The
  presence of a `PrintHost` or printer-agent interface does not imply that every
  provider supplies full telemetry. Keep credentials out of status results.

DoD: cover no selected printer, offline devices, missing telemetry, and
agent switching. Compare supported fields with the device UI. Use simulated
status data for automated checks and document verification on available hardware.
An unavailable device service raises the contracted error; it is distinguishable
from an initialized service with no known printers.

## Milestone 4: Mass estimates and release verification

### PY-09 — Estimate solid geometry mass

Priority: P1. Dependencies: PY-04, PY-06.

- Offer an explicitly labeled solid geometry estimate using
  `mass_g = volume_mm3 * density_g_cm3 / 1000`, with density supplied by the caller
  or resolved from the applicable material configuration.
- Apply volume and instance transforms, including nonuniform scaling and
  mirroring, and count the requested instances. Exclude non-part modifiers;
  account for negative volumes and overlapping parts using existing geometry
  facilities, or return an incomplete estimate with a reason. Approximate partial
  sums are not a substitute for the requested scope's total.
- Report missing density, invalid or uncomputed mesh volume, and ambiguous
  material assignment instead of silently returning a plausible number.

DoD: analytic solids establish expected volume and mass; scaling and
copies change mass correctly. Cases involving open meshes, overlapping parts,
negative volumes, and multiple materials disclose their limitations. Results
identify density source, scope, units, and estimation method. An incomplete scope
has `mass_g=None`; a partial sum is never presented as its total.
RED-N08 consumes the retained native project after its temporary source is
removed; no reimport or reconstruction from reported metadata is permitted.

### PY-10 — Start and observe slicing jobs

Priority: P1. Dependencies: PY-02, PY-06.

- Expose slicing for a specified plate using the existing application slicing
  workflow and resolved configuration. Provide job state, progress, failure,
  completion, and cooperative cancellation.
- Define behavior for an already-running job and for project edits/reordering
  during a job. Associate results with the project/plate/configuration used so
  later edits cannot make old results appear current.
- Complete estimation without uploading to a printer or starting a print. Any
  intermediate G-code follows existing temporary-file cleanup conventions.

DoD: success, failure, cancellation, and edit-during-slice cases leave the
application usable and expose no partial result as final. Equivalent inputs use
the same slicing behavior as the GUI. Result retrieval follows the documented
timeout and UI-thread rules; jobs retain their immutable input snapshots.
RED-N08 verifies that workers receive retained native model/config/plate data or
an isolated native copy and can finish after the source file disappears.

### PY-11 — Expose estimates derived from slicing

Priority: P1. Dependencies: PY-10.

- Expose completed, valid slicing statistics with units and scope: consumed
  filament length, volume, mass, and estimated time, including per-material data
  where available. Reuse native statistics and density calculations.
- Distinguish finished-part material, supports, purge/wipe tower, and total
  consumption where native accounting supports the distinction. Investigate
  category semantics before labeling them; return unavailable values for any
  breakdown the existing pipeline cannot establish reliably.
- Define per-plate versus per-object availability. Identify cached statistics
  loaded from a 3MF separately from results freshly computed for current settings.

DoD: totals match the GUI/native statistics within stated tolerances.
Demonstrate infill, wall, support, and multi-material cases; show how solid mass,
part mass, and total consumption differ. Invalidated results require a fresh
slice or an explicit stale-result read.

### PY-12 — Publish examples and complete compatibility verification

Priority: P1. Dependencies: all shipped items. Run the Gate A subset with PY-13;
complete the full item only after PY-03 through PY-11.

- Update binding docstrings and generated stubs using the existing tooling. Add
  examples for reading a 3MF, traversing settings and geometry, listing devices,
  reordering plates, and calculating both kinds of mass estimate.
- Document the embedded execution environment and lifetime/cancellation rules.
  The existing importable stub-generation module is not a supported standalone
  Python distribution or a connection to a running GUI.
- Verify the integrated shell workflow on Windows, macOS, and Linux, including
  UI layout/DPI behavior. New top-level windows follow `SetSizerAndFit` conventions.
  Record any unavailable platform/provider checks explicitly.

DoD: examples work against the implemented API; existing plugin tests
remain green; normal startup and slicing work with the shell unused. Existing
projects/profiles round-trip without a new format requirement. Any unavoidable
format change has migration handling and separate compatibility verification.
Publish the required Gate A/Gate B evidence. Missing platform checks remain open
DoD items; recording their absence does not count as a pass.

## Verification placement

Follow [tests/AGENTS.md](../../tests/AGENTS.md) and reuse suite helpers.

| Behavior | Verification home |
| --- | --- |
| Python API contracts, exceptions, object lifetime, runtime integration | `tests/slic3rutils/` |
| CLI arguments, process exit, stdout/stderr, display independence | Executable-level integration cases driven by the existing test setup |
| Independent 3MF reads, geometry calculations, configuration data | `tests/libslic3r/` |
| Estimates requiring slicing, material accounting, invalidation | `tests/fff_print/` |
| Shell input/output, responsiveness, undo/redo, plate UI synchronization | Targeted GUI integration checks or documented manual scenarios |
| Device snapshots and unavailable/stale state | Binding tests with simulated device data; documented provider checks |

Keep numerical tests tied to explicitly chosen dimensions, densities, and print
settings. Test behavior rather than exact G-code formatting. Add tests alongside
each implementation item instead of postponing all verification to PY-12.

## Follow-up candidates

- An interactive terminal REPL and a standalone importable Python distribution;
  the CLI script runner is already part of Gate A.
- Device services in a process without a GUI, or a connection to an already
  running Orca instance; these are distinct from the initial file-inspection CLI.
- Object arrangement within/across plates, separately from plate ordering.
- Project save/export, object transforms, and configuration edits through Python
  operations that preserve undo and slicing invalidation.
- Project/device event subscriptions after polling and snapshot semantics settle.
- Broader printer-provider telemetry support where current integrations lack it.

Remote RPC, arbitrary printer commands, and parity with every application control
are not part of the initial delivery.
