"""Sphinx configuration for the OrcaSlicer Python API documentation.

The reference pages under ``generated/`` are produced by ``sphinx_doc_gen.py``
running inside OrcaSlicer; the pages beside this file are written by hand.
Nothing here imports ``orca``, so the HTML can be built on a machine that has no
OrcaSlicer build — as long as ``generated/`` has been produced somewhere.
"""

from __future__ import annotations

import re
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def read_app_version() -> str:
    """Take the version from version.inc so the docs cannot drift from the app."""
    version_inc = REPO_ROOT / "version.inc"
    try:
        match = re.search(r'set\(SoftFever_VERSION\s+"([^"]+)"\)', version_inc.read_text(encoding="utf-8"))
    except OSError:
        return "unknown"
    return match.group(1) if match else "unknown"


# -- Project ----------------------------------------------------------------

project = "OrcaSlicer Python API"
author = "OrcaSlicer contributors"
copyright = "OrcaSlicer contributors"  # noqa: A001 - Sphinx expects this name

# The application version these docs were generated from.
release = read_app_version()
version = release
# The host API contract version, which moves independently of the app version.
# Keep in step with orca.host.api_version and change_log.rst.
host_api_version = "1.0"

rst_epilog = f"""
.. |app_version| replace:: {release}
.. |host_api_version| replace:: {host_api_version}
"""

# -- General ----------------------------------------------------------------

extensions = [
    "sphinx.ext.intersphinx",
    "sphinx.ext.extlinks",
]

exclude_patterns = ["_build", "Thumbs.db", ".DS_Store", "README.md"]
default_role = "py:obj"
primary_domain = "py"
nitpicky = False
add_module_names = False
toc_object_entries_show_parents = "hide"

intersphinx_mapping = {"python": ("https://docs.python.org/3", None)}

extlinks = {
    "repo": ("https://github.com/OrcaSlicer/OrcaSlicer/blob/main/%s", "%s"),
}

# -- HTML -------------------------------------------------------------------

# furo gives the left navigation tree and right "on this page" column this kind
# of reference needs. Fall back to the bundled theme so a build without it still
# succeeds rather than failing on a missing dependency.
try:
    import furo  # noqa: F401

    html_theme = "furo"
    html_theme_options = {
        "navigation_with_keys": True,
        "source_repository": "https://github.com/OrcaSlicer/OrcaSlicer/",
        "source_branch": "main",
        "source_directory": "docs/python_api/",
    }
except ImportError:  # pragma: no cover - depends on the environment
    html_theme = "alabaster"
    html_theme_options = {"page_width": "1100px", "sidebar_width": "260px"}

html_title = f"OrcaSlicer {release} Python API"
html_short_title = "OrcaSlicer Python API"
html_static_path = ["_static"]
html_css_files = ["orca.css"]
html_show_sourcelink = False
html_copy_source = False
