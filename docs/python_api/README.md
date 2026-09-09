# OrcaSlicer Python API documentation

Sphinx sources for the `orca` Python API reference.

The pages come from two places:

- **Narrative pages** (`index.rst`, `info_*.rst`, `change_log.rst`) are written by
  hand and live in git.
- **Reference pages** (`generated/`) are produced by `sphinx_doc_gen.py`, which
  introspects the live `orca` module. They are not committed.

`orca` is compiled into the OrcaSlicer binary and only exists inside that
process, so the generator has to run there — the same way Blender generates its
Python API docs from inside Blender. That is also what keeps the reference honest:
a signature or docstring that is not in the binary cannot appear in the docs.

## Build

```sh
python3.12 -m venv .venv-docs                              # from the repo root
.venv-docs/bin/pip install -r docs/python_api/requirements.txt
```

Then, from this directory:

```sh
make reference ORCA=../../build/arm64/src/Release/OrcaSlicer.app/Contents/MacOS/OrcaSlicer
make html
open _build/html/index.html
```

`make` picks up `.venv-docs` from the repository root automatically, so there is
nothing to activate. Point it elsewhere with
`make html SPHINXBUILD=/path/to/sphinx-build`.

`make reference` needs a built OrcaSlicer; `make html` does not, so a machine
with `generated/` already present can build the site without a compiler.

On other platforms, point `ORCA` at the built executable:

| Platform | Typical path |
| --- | --- |
| macOS | `build/arm64/src/Release/OrcaSlicer.app/Contents/MacOS/OrcaSlicer` |
| Linux | `build/src/OrcaSlicer` |
| Windows | `build\src\Release\OrcaSlicer.exe` |

The `html` target uses `-W --keep-going`, so a broken cross-reference fails the
build instead of shipping a dead link.

## Adding to the docs

- **New bindings** need no work here beyond a docstring on the C++ side. Rerun
  `make reference` and they appear. A property docstring ending in
  `\n\n:type: <T>` renders as a type field.
- **A new module** must be added to `MODULE_ORDER` and `TITLES` in
  `sphinx_doc_gen.py`. This is deliberate: bindings do not silently grow a page.
- **Narrative changes** go in the `info_*.rst` files. Keep them consistent with
  [the execution contract](../backlog/python-api-contract.md), which is normative.
- **API changes** belong in `change_log.rst`, against the host API version from
  `orca.host.api_version` — not the application version.
