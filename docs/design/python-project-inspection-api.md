# Python project inspection API

Date: 2026-09-08

Status: Implemented for file reads, with the draft acceptance cases in
[project_inspection_contract.py](examples/project_inspection_contract.py) still
unrun because their fixtures are not authored yet. `snapshot()` remains Gate B.
This design refines PY-01, PY-02, PY-03, PY-04, and PY-06 in the
[Python API backlog](../backlog/python-api-and-shell.md).

The [execution contract and DoD](../backlog/python-api-contract.md) define CLI
arguments, version/capability discovery, shared exceptions, and release gates.
The first delivery runs inspection scripts from the Orca executable without a
GUI; the in-app shell follows.

The [native-loading contract](../backlog/python-api-contract.md#native-loading-and-automation-contract)
is mandatory: an owned native project powers these views and later native
automation operations. A separate archive-metadata reader does not implement this
API. See the [RED review and plan](../backlog/python-api-red-plan.md) for the
architecture review, native comparisons and executable evidence required.

The implementation lives in
[PluginHostProject](../../src/slic3r/plugin/host/PluginHostProject.hpp) (the owned
snapshot) and
[PluginHostProjectBindings.cpp](../../src/slic3r/plugin/host/PluginHostProjectBindings.cpp)
(the Python views over it).

## Entry point and intended use

Add `orca.host.project` alongside the existing `orca.host` bindings:

```python
import orca

project = orca.host.project.read("assembly.3mf")

for plate in project.plates:
    print(plate.index, plate.name)
    for instance in plate.instances:
        transform = instance.transform(space="plate")
        bounds = instance.bounds(space="plate")
        print(instance.object.name, instance.id)
        print("size mm:", bounds.size if bounds.defined else None)
        print("position mm:", transform.position_mm)
        print("rotation degrees:", transform.rotation_deg)
        print("materials:", [m.name for m in instance.materials().items])
```

This is the convenient path for a file with recoverable plate frames. A more
complete [inspection example](examples/inspect_3mf.py) also handles unassigned
instances, unknown plate coordinates, missing materials, and JSON output.

```sh
OrcaSlicer --file assembly.3mf --script docs/design/examples/inspect_3mf.py -- --compact
```

`read()` returns an owned, read-only project snapshot. It does not open a project
in the GUI, use the GUI's current presets, arrange geometry, or slice a model.
It requires an initialized embedded Orca Python runtime, but the file-reading
operation itself must not require a wx application. This does not promise an
installable standalone `orca` package.

`snapshot()` captures the current GUI document into the same snapshot types. It
requires a running application and copies mutable data on the owning thread.
The capture must be coherent; a conflicting operation fails with `BusyError`.

## Namespace and object graph

| Surface | Purpose |
| --- | --- |
| `orca.host.project.read(path)` | Inspect a 3MF independently of the GUI document |
| `orca.host.project.snapshot()` | Inspect a fixed copy of the current GUI document |
| `Project.plates` | Plates in project order, including empty plates |
| `Project.objects` | Unique object definitions, each containing volumes |
| `Project.instances` | Every placed copy, including unassigned/nonprintable copies |
| `Plate.instances` | Copies assigned to this plate |
| `Instance.object` | Shared object definition used by this copy |
| `Object.volumes` | Parts, negative volumes, parameter modifiers, and support modifiers |
| `Project.materials` | All known FFF filament slots, including unused slots |
| `Instance.material_assignment(volume)` | Default and painted slot references for a part |
| `Instance.materials()` / `Plate.materials()` | Deduplicated geometry material references and completeness |

Group **instances**, not object definitions, by plate. Two instances of one object
can belong to different plates. A multipart object can use several materials.
Names need not be unique; use IDs for joins and names for display.

The snapshot types live inside `orca.host.project`. They do not replace or change
the existing `orca.host.Model`, `ModelObject`, `ModelVolume`, and `ModelInstance`
classes. Separate wrappers are justified by their owning lifetime and project
metadata; they reuse native models, geometry, and immutable mesh storage.
Internally retain native configurations and plate relationships as well as the
model. Later native operations must access this retained state directly or make
a native copy; they must not rebuild it from serialized inspection properties or
reopen `source_path`. Internal native test access need not expose C++ pointers in
the public Python API.

## Proposed signatures

The following is an interface sketch, not an implementation. All attributes and
collections are read-only. Sequences are tuples; mappings reject mutation.
`BoundingBox` and `TriangleMesh` reuse existing `orca.host` value/mesh bindings.

```python
from __future__ import annotations

from os import PathLike
from typing import Literal, Mapping
from orca.host import BoundingBox, TriangleMesh, ModelVolumeType
from orca.host.errors import (
    ProjectReadError, UnsupportedProjectError, ApplicationUnavailableError,
    BusyError, CoordinateUnavailableError,
)

Vec3 = tuple[float, float, float]
Matrix4 = tuple[tuple[float, float, float, float],
                tuple[float, float, float, float],
                tuple[float, float, float, float],
                tuple[float, float, float, float]]
Space = Literal["world", "plate"]

def read(path: str | PathLike[str]) -> Project: ...
def snapshot() -> Project: ...

class Project:
    source_path: str | None
    source_kind: Literal["file", "gui"]
    source_document_id: str | None
    source_revision: int | None
    metadata: Mapping[str, str]
    config: Mapping[str, str]
    plates: tuple[Plate, ...]
    objects: tuple[Object, ...]
    instances: tuple[Instance, ...]
    unassigned_instances: tuple[Instance, ...]
    materials: tuple[Material, ...]
    issues: tuple[Issue, ...]

    def material(self, slot_id: int) -> Material: ...

class Plate:
    id: str
    index: int
    name: str
    locked: bool | None
    config: Mapping[str, str]
    transform_to_world: Transform | None
    instances: tuple[Instance, ...]

    def materials(self) -> MaterialSummary: ...

class Object:
    id: str
    name: str
    config: Mapping[str, str]
    volumes: tuple[Volume, ...]
    instances: tuple[Instance, ...]
    bounds: BoundingBox

class Volume:
    id: str
    name: str
    type: ModelVolumeType
    config: Mapping[str, str]
    transform: Transform
    bounds: BoundingBox

    def mesh(self) -> TriangleMesh: ...

class Instance:
    id: str
    object: Object
    plate_id: str | None
    printable: bool

    def transform(self, *, space: Space) -> Transform: ...
    def bounds(self, *, space: Space) -> BoundingBox: ...
    def material_assignment(self, volume: Volume) -> MaterialAssignment: ...
    def materials(self) -> MaterialSummary: ...

class Transform:
    matrix: Matrix4
    position_mm: Vec3
    rotation_deg: Vec3 | None
    scale: Vec3 | None
    mirror: tuple[int, int, int] | None

class Material:
    slot_id: int
    name: str | None
    preset_name: str | None
    material_type: str | None
    vendor: str | None
    color: str | None
    density_g_cm3: float | None
    diameter_mm: float | None
    kind: Literal["physical", "mixed", "unknown"]
    component_slot_ids: tuple[int, ...]
    field_sources: Mapping[str, str]
    config: Mapping[str, str]

class MaterialAssignment:
    default_slot_id: int | None
    painted_slot_ids: tuple[int, ...]
    is_painted: bool
    source: Literal["volume", "object", "project_default", "unknown", "not_applicable"]
    complete: bool

class MaterialSummary:
    items: tuple[Material, ...]
    complete: bool
    issues: tuple[Issue, ...]

class Issue:
    code: str
    message: str
    entity_id: str | None

# Imported error names are re-exported by orca.host.project as identical aliases.
```

## Ownership, ordering, and errors

- Snapshots own their model, plate data, configurations, and metadata. Child
  wrappers retain the snapshot storage without creating Python reference cycles.
  A retained instance or mesh remains usable after `del project`, loading another
  file, or changing the live GUI. Files are closed before `read()` returns;
  there is no `close()` or context-manager requirement.
- Precompute mutable geometry caches during loading/capture or synchronize lazy
  cache updates before allowing concurrent reads of a snapshot.
- IDs are opaque and unique across entities within one snapshot. They remain
  stable during that snapshot's lifetime; they are not C++ pointer addresses,
  sequence indexes, or promised persistent IDs across reads. Do not join snapshots
  using these IDs. No original archive resource IDs are promised in version one.
- File reads set `source_document_id` and `source_revision` to None. GUI captures
  carry the document identity and a monotonic mutation token; undo/redo also
  advance it. Later GUI operations validate both values as specified in the
  backlog contract. These tokens do not make entity IDs persistent across reads.
- Plate indexes are zero-based sequence positions. Material slot IDs stay
  one-based. `project.material(1)` means slot 1, while `project.materials[0]` means
  the first material in slot-ID order. Slots need not be contiguous.
- Objects and instances retain native import order. A plate's instances are the
  project instance sequence filtered by membership. Every instance appears on
  exactly one plate or in `unassigned_instances`, never both. Conflicting or
  broken membership yields an issue and an unassigned instance.
- `Instance.printable` is the imported per-instance toggle. It does not certify
  bed containment, successful validation, or the existence of slicing results.
- An archive without plate metadata yields `plates == ()`, all instances
  unassigned, and `plate_metadata_missing`. Do not invent a printer bed or infer
  ownership from proximity. Empty named plates remain present.
- A recognized part-to-slot reference with absent slot metadata creates an
  `unknown` material record for that ID. Its fields are `None` and an issue is
  recorded. A wholly unknown assignment does not invent slot 1.
- Missing files and permission failures use `FileNotFoundError`/`PermissionError`.
  Corrupt archives use `ProjectReadError`; unsupported format/version requirements
  use `UnsupportedProjectError`. Recoverable native substitutions become issues.
  The file is never rewritten. `material()` raises `KeyError` for an unknown ID.
- Wrong argument types use `TypeError`; an unknown coordinate space or a volume
  from another object/snapshot uses `ValueError`. Missing plate membership/frame
  uses `CoordinateUnavailableError`. `snapshot()` without a GUI uses
  `ApplicationUnavailableError`.

## Coordinates, size, rotation, and position

Distances are in millimeters, including files whose declared units require native
conversion. `Volume.bounds` is in mesh-local space. `Volume.transform` maps that
mesh to object coordinates. `Object.bounds` is the object-space envelope of model
parts after volume transforms, before any instance transform.

`instance.transform(space="world")` maps object coordinates to the native
imported project's world frame. `Plate.transform_to_world` maps the plate's bed
frame into that frame. Plate axes/origin follow native bed coordinates; do not
assume the origin is always the bed center or its lower-left corner.

```text
world_vertex = instance_world_matrix @ volume_matrix @ local_vertex
plate_vertex = inverse(plate_to_world_matrix) @ world_vertex
instance_plate_matrix = inverse(plate_to_world_matrix) @ instance_world_matrix
```

Matrices are immutable row tuples representing 4x4 affine matrices acting on
column vectors `(x, y, z, 1)`. Translation is in the last column. Ordinary scalar,
tuple, and bounds reads do not require NumPy. Existing mesh-array methods do.

Plate frames are recovered with native project/bed rules using file data, not
current GUI presets or a newly arranged plate grid. The application lays plates on
a square-ish grid whose cell is the bed size plus a fixed fraction of it, and
stores instances in that grid's frame; the reader reproduces that layout from the
project's own `printable_area`, through the rule
[PartPlateList shares with it](../../src/slic3r/GUI/PlateGrid.hpp). Columns follow
`compute_colum_count(plate_count)`, so a third plate starts a second row rather
than continuing the first. Without a usable printable area the frame cannot be
recovered: `transform_to_world` is `None` and the project reports
`plate_frame_unavailable`. World reads still work; plate-space reads raise
`CoordinateUnavailableError`. Imported/recentered native coordinates are the API's
source of truth, not the original XML transform text.

`position_mm` is the transformed object origin, which may differ from the bounds
minimum and center. `bounds(space=...).size` is the axis-aligned envelope **after**
rotation, scale, and mirroring. It is not an oriented bounding box or the raw mesh
size. Bounds use transformed model-part vertices, not just transformed corners of
an earlier bounding box. Modifiers and negative volumes do not enlarge them.
They describe the positive-part envelope, not a boolean-subtracted final solid.
Empty geometry produces `bounds.defined == False`; inspect that before its values.

For decomposable transforms, angles are degrees in `(x, y, z)` order, consistent
with the native convention:

```text
M = T(position) @ Rz(z) @ Ry(y) @ Rx(x) @ S(scale) @ S(mirror)
```

Scale components are nonnegative and mirror components are `-1` or `+1`. Euler
decompositions are not unique; validate the reconstructed matrix rather than a
particular Euler branch. For shear or a singular transform that cannot be
represented faithfully, `rotation_deg`, `scale`, and `mirror` are `None` together.
The full matrix and position remain available. This explicitly named degree
property does not change the existing raw binding's radian-valued `rotation()`.

## Materials and settings

Version one normalizes **FFF filament slots**. A slot is not a physical nozzle,
extruder, AMS bay, or generic 3MF base-material resource. Generic material/color
resources and SLA resin need separate future schemas; retain native information
and report unsupported normalization instead of guessing filament assignments.

- `Project.materials` includes configured slots and identifiable referenced slots.
  Keep duplicate names/colors as separate slots. `name` is the imported display
  name, `preset_name` the profile label, and `material_type` a value such as `PLA`.
  Do not parse a brand or polymer from a profile name.
- Typed fields use project configuration first, then the corresponding embedded
  preset for missing fields, then cached slice metadata where it contains that
  field. File reading never fills gaps from installed/user-selected presets.
  Record conflicting data as an issue. `field_sources` records each typed field
  as `project_config`, `embedded_preset`, `slice_metadata`, or `unavailable`.
  For `snapshot()`, capture the GUI's current material configuration and report
  `gui_config` for fields obtained there, including unsaved edits.
- `color` is normalized to `#RRGGBB` or `#RRGGBBAA` when valid. Unknown strings,
  unavailable values, and invalid/nonpositive density or diameter yield `None`
  plus an issue where appropriate. Do not substitute a default density.
- `material_assignment(volume)` resolves native volume/object/default assignment
  rules within this snapshot. Native `extruder == 0` inheritance is resolved, not
  exposed as a valid slot. Return `source="project_default"` only when the file's
  FFF configuration supplies the context for that default.
- For non-part volumes the assignment is `not_applicable`, with no slot IDs and
  `complete=True`. For painted parts report the default separately from referenced
  painted slots. If their set cannot be fully decoded, preserve `is_painted=True`,
  mark `complete=False`, and emit `material_assignment_incomplete`.
- `materials()` aggregates model-part default and painted references, sorted and
  deduplicated by slot ID, including nonprintable copies. It is a conservative
  reference inventory, not proof every referenced slot will be extruded. The
  `complete` flag describes reference resolution, not completeness of density or
  other metadata. An empty plate has an empty, complete inventory.
- Mixed slots retain their virtual ID and component IDs. Include the referenced
  virtual slot in the inventory, and let callers follow its components explicitly.
  Do not silently replace it or compute density from an unspecified mixing ratio.
- Supports, purge, and actual consumed grams are outside this reference inventory.
  Those belong to the slicing-statistics API in PY-11.

`config` mappings preserve serialized native setting values at their own scope.
For example, `object.config.get("layer_height")` reads its override, not the
effective value inherited from the plate/project. Material configs contain the
available settings for that slot, with vector entries selected by slot. Missing
keys remain missing. Typed normalized material fields are separate from these
serialized mappings. A general effective-config API is a later extension using
native configuration resolution.

## Acceptance cases and planned fixtures

[project_inspection_contract.py](examples/project_inspection_contract.py) expresses
the behavior as Python assertions. It accepts the namespace and a fixture
directory; it does not mock the API. **The fixtures below are still unauthored, so
these twelve cases have never run.** They must be authored with the native writer
and checked after a writer/reader round trip. Numerical expectations describe the
normalized native scene, not pre-import XML values.

Its `plates_and_materials.3mf` plate origins need correcting before the cases can
run: three plates occupy a two-column grid, so the third plate starts a second row
instead of continuing the first, and the stride is the bed size plus the layout
gap rather than the bed size alone. Expected origins have to be derived from the
[grid rule](../../src/slic3r/GUI/PlateGrid.hpp) and the fixture's own bed, not
assumed to be evenly spaced along X.

The repository also contains a real Orca/Bambu project fixture at
[`test_files/test_file_01.3mf`](../../test_files/test_file_01.3mf) and an executable
acceptance script at
[`test_files/inspect_test_file_01.py`](../../test_files/inspect_test_file_01.py).
This file has two plates, three object definitions, and three instances. Plate 0
contains `3DBenchy.drc`; plate 1 contains `OrcaCube_v2.drc` and
`OrcaPlug_v2.drc`. All use material slot 1, while all five configured slots must
remain visible in `Project.materials`. Its assertions exercise native normalized
plate/world transforms, geometry bounds, and material fields through the public
API. The script must never read the archive XML itself. Passing this report check
does not establish native loading: RED-N01–N07 remain mandatory. The absolute
plate frames are checked against `PartPlateList`'s own origins in
[test_project_inspection_contract.cpp](../../tests/slic3rutils/test_project_inspection_contract.cpp);
the earlier 432 mm stride was simply wrong, and the real value for that fixture's
350 mm bed is 420 mm.

| Planned fixture | Required content and expected observations |
| --- | --- |
| `plates_and_materials.3mf` | Plates `Left`, `Right`, `Empty`; world origins `(0,0,0)`, `(300,0,0)`, `(600,0,0)` with identity orientation. Two unique objects: `Cube` and `Bracket`. `Left` contains one copy of each; `Right` contains another `Cube`; `Empty` has none. No unassigned instances. |
| Same fixture: geometry | Cube local bounds `(-5,-10,-15)` to `(5,10,15)`, one part with identity volume transform. Left copy: position `(50,60,15)`, Z rotation 90 degrees, unit scale. Its plate bounds are `(40,55,0)` to `(60,65,30)`. Right copy: plate position `(20,30,7.5)`, zero rotation, scale `(2,1,0.5)`, X mirror; size `(20,20,15)`, world position `(320,30,7.5)`. Bracket has part volumes `Body` and `Cap`, plus a large outlying parameter modifier excluded from its part bounds/materials. |
| Same fixture: materials/settings | Slots 1–4: `PLA Red` (1.24 g/cm3), `PETG Black` (1.27), `PLA White` (1.20), unused `TPU` (1.21). Cube inherits slot 1 from its object. Bracket Body inherits slot 2; Cap overrides to slot 3; modifier has an ignored slot 4 override. All assignments unpainted. Right has `printable=False` and retains membership/materials. Project `layer_height="0.2"`, Left override `"0.16"`; Cube has no layer-height override. |
| `generic_geometry.3mf` | One build instance, valid geometry, no plate or FFF configuration metadata. Plates/materials are empty; membership and assignment remain unknown. |
| `missing_density.3mf` | One FFF slot with explicit type/color/diameter but density absent from project, embedded preset, and cached metadata. Typed density is `None`. |
| `painted_incomplete.3mf` | A part with known default slot 1 and detected painting whose complete referenced slot set cannot be recovered. Inventory is explicitly incomplete. |
| `sheared_instance.3mf` | A valid instance with a nonsingular XY shear of 0.25. Matrix is preserved; TRS convenience decomposition is unavailable. |
| `corrupt.3mf` | Invalid archive; reading raises `ProjectReadError`. |

Additional implementation checks: empty meshes, duplicate names, non-mm units,
nonidentity volume transforms, non-90-degree rotations, missing plate frames,
broken/conflicting membership, sparse/mixed slots, Unicode paths, missing files,
unsupported versions, NumPy immutability, and child lifetimes after GUI replacement.
Add a GUI check that `read()` leaves the active project and presets unchanged and
that `snapshot()` remains fixed after subsequent edits.

## Native implementation mapping

| Proposed behavior | Reuse / required work |
| --- | --- |
| Owned project read | [Model::read_from_archive](../../src/libslic3r/Model.cpp) plus [PlateData](../../src/libslic3r/Format/bbs_3mf.hpp) and embedded presets, retained by `ProjectSnapshot`. |
| Instance grouping | The importer kept only `obj_inst_map`, which is keyed by object resource id and therefore collapses several copies of one object on a plate. It now also records every `<model_instance>` element and resolves it to `PlateData::objects_and_instances`, so membership is complete. Plate frames come from the shared grid rule, with no GUI window. |
| Bounds and transforms | [Model geometry methods](../../src/libslic3r/Model.cpp) and [Geometry transforms](../../src/libslic3r/Geometry.cpp); reuse [mesh bindings](../../src/slic3r/plugin/host/PluginHostMesh.cpp). |
| Slot assignment | Native model/config resolution and painted/mixed material metadata; do not equate `extruder_id()` with physical nozzle identity. |
| GUI snapshot | Capture model, plates, and matching configuration together on the owning thread; share only immutable mesh storage with ongoing slicing. |
| Verification | Python boundary cases in `tests/slic3rutils/`, native import/geometry cases in `tests/libslic3r/`, according to [tests/AGENTS.md](../../tests/AGENTS.md). Reuse the existing embedded interpreter helper; no new test framework is required. |

Deliver file reading, grouping, and basic metadata first; then complete coordinate
and material edge cases. The shell consumes this same namespace. Mutation, plate
reordering, device status, mass estimation, and slicing commands keep their own
backlog items and do not alter the immutable inspection contract.
