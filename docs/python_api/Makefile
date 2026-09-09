# Documentation build for the OrcaSlicer Python API.
#
#   make reference ORCA=/path/to/OrcaSlicer   regenerate generated/ from the app
#   make html                                 build HTML into _build/html
#   make clean
#
# The reference pages need a built OrcaSlicer, because `orca` only exists inside
# that process. The HTML build itself does not.

# Prefer the repo-local documentation venv when it exists, so the documented
# workflow needs no `activate` step; fall back to whatever is on PATH.
VENV_SPHINX := $(wildcard $(CURDIR)/../../.venv-docs/bin/sphinx-build)
SPHINXBUILD ?= $(if $(VENV_SPHINX),$(VENV_SPHINX),sphinx-build)
SOURCEDIR   := .
BUILDDIR    := _build
ORCA        ?=

.PHONY: help html reference clean linkcheck

help:
	@echo "make reference ORCA=<path to OrcaSlicer executable>  regenerate generated/"
	@echo "make html                                            build HTML"
	@echo "make linkcheck                                       check external links"
	@echo "make clean                                           remove _build and generated"

reference:
	@if [ -z "$(ORCA)" ]; then \
		echo "set ORCA to the OrcaSlicer executable, e.g."; \
		echo "  make reference ORCA=../../build/arm64/src/Release/OrcaSlicer.app/Contents/MacOS/OrcaSlicer"; \
		exit 2; \
	fi
	"$(ORCA)" --script "$(CURDIR)/sphinx_doc_gen.py" -- --output "$(CURDIR)/generated"

html:
	@if [ ! -f generated/index.rst ]; then \
		echo "generated/ is missing; run 'make reference ORCA=...' first"; \
		exit 2; \
	fi
	@command -v "$(SPHINXBUILD)" >/dev/null 2>&1 || { \
		echo "$(SPHINXBUILD) not found. From the repository root:"; \
		echo "  python3.12 -m venv .venv-docs"; \
		echo "  .venv-docs/bin/pip install -r docs/python_api/requirements.txt"; \
		exit 2; \
	}
	$(SPHINXBUILD) -b html -W --keep-going "$(SOURCEDIR)" "$(BUILDDIR)/html"
	@echo "built $(BUILDDIR)/html/index.html"

linkcheck:
	$(SPHINXBUILD) -b linkcheck "$(SOURCEDIR)" "$(BUILDDIR)/linkcheck"

clean:
	rm -rf "$(BUILDDIR)" generated
