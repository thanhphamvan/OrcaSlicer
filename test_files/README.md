# Python project inspection acceptance fixture

`test_file_01.3mf` is the first real-file acceptance fixture for the proposed
OrcaSlicer Python project API. It contains three object definitions and three
instances across two plates:

| Plate index | Instances | Assigned material slots |
| --- | --- | --- |
| 0 | `3DBenchy.drc` | 1 |
| 1 | `OrcaCube_v2.drc`, `OrcaPlug_v2.drc` | 1 |

“Object” means a unique model definition; “instance” means a placed copy. This
fixture has one instance per object. Positions below are stored build-item origins
in mm; sizes were observed through native `OrcaSlicer --info`. These observations
are a behavior baseline, not proof that the Python API loads the project natively.

| Object | Stored position mm | Size mm | Rotation deg | Scale |
| --- | --- | --- | --- | --- |
| `3DBenchy.drc` | `(175, 160, 24)` | `(60.001007, 31.003998, 48)` | `(0, 0, 0)` | `(1, 1, 1)` |
| `OrcaCube_v2.drc` | `(606.999252, 160, 15)` | `(30, 29.999992, 30)` | `(0, 0, 0)` | `(1, 1, 1)` |
| `OrcaPlug_v2.drc` | `(579, 160.000001, 4.898826)` | `(21.998505, 21.999252, 9.797651)` | `(0, 0, 0)` | `(1, 1, 1)` |

The earlier 432 mm plate offset was wrong. The project's bed is 350 x 320 mm, and
plates are laid out on a grid whose cell is the bed plus a fifth of it, so plate 1
sits at x = 350 x 1.2 = **420 mm**. The API reports that origin, which puts the
plate-1 pair's combined envelope centre at x = 175 mm — the bed centre, matching
where plate 0's single object sits. That consistency is a property of the fixture,
not of the formula. The script itself checks world/plate matrix composition and
never hardcodes a stride; production code must not infer a plate frame from the
expected positions either.

The project config defines five material slots. Slot 1 is used by all three
parts; slots 2–5 remain part of the project inventory and are intentionally
unused. The fixture is 3,107,978 bytes and its SHA-256 is
`482ec156ee1bf835165fd91c0f2aadf8ed3d695690701c82665ceec0222ba705`.

| Slot | Preset | Type | Vendor | Color | Density g/cm³ | Diameter mm |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | `eSUN PLA+ @BBL H2D` | PLA | eSUN | `#057748` | 1.25 | 1.75 |
| 2 | `Generic PETG @BBL H2D` | PETG | Generic | `#FFF144` | 1.27 | 1.75 |
| 3 | `Bambu PETG Translucent @BBL H2D 0.4 nozzle` | PETG | Bambu Lab | `#0ACC38` | 1.25 | 1.75 |
| 4 | `Generic PLA @BBL H2D` | PLA | Generic | `#F72323` | 1.24 | 1.75 |
| 5 | `Generic PLA @BBL H2D` | PLA | Generic | `#161616` | 1.24 | 1.75 |

After Gate A is implemented, run the checked-in RED acceptance script with:

```sh
OrcaSlicer --file test_files/test_file_01.3mf \
  --script test_files/inspect_test_file_01.py -- --verify
```

The script exits 0 and prints one valid JSON document to stdout. It reads the
file exclusively through `orca.host.project.read()` and verifies plate grouping,
object/instance counts, world and plate-local positions, scale, rotation,
mirroring, bounds, and material metadata. It does not parse the 3MF archive
directly.

For a process-level check of two different user scripts, named and positional
input, Unicode paths, script arguments and source preservation, run:

```sh
python3 test_files/run_inspection_red.py --orca-bin /path/to/OrcaSlicer
```

The CLI acceptance cases CLI-01 to CLI-12 have their own controller:

```sh
python3 test_files/run_cli_acceptance.py --orca-bin /path/to/OrcaSlicer
```

It covers argument shapes, the `--` delimiter, Unicode and spaced paths, exit
codes, stream separation, and Ctrl+C, and prints one PASS/FAIL/SKIP line per case.
A SKIP is not a pass: it names what could not run here and leaves that case open.

Both launch the real executable. Running a script with bundled Python and
importing `orca.so` only probes the binding boundary; it does not verify CLI
execution. Both controllers are manual tests pending CTest/CI integration.

The [RED review and plan](../docs/backlog/python-api-red-plan.md) also requires
native C++ mesh/transform comparisons, retained-project ownership and source
review of the loading path; those live in
[`tests/slic3rutils/test_project_inspection_contract.cpp`](../tests/slic3rutils/test_project_inspection_contract.cpp).
Matching this JSON report alone cannot rule out a separate ZIP/XML implementation.
Existing native importers are expected to parse ZIP/XML; the API reuses them and
retains their native project state.
