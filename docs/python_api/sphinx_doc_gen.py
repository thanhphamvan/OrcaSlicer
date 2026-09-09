"""Generate reStructuredText for the ``orca`` Python API by introspecting it.

This runs inside OrcaSlicer, because ``orca`` is an embedded module that only
exists in that process — there is no installable package to import from an
ordinary interpreter:

    OrcaSlicer --script docs/python_api/sphinx_doc_gen.py -- --output generated

The reference pages are written from the live module, so they cannot drift from
the shipped bindings: a signature or docstring that is not in the binary does not
appear in the documentation. The narrative pages next to this script are written
by hand and are not touched here.
"""

from __future__ import annotations

import argparse
import inspect
import keyword
import re
import shutil
import sys
import types
from pathlib import Path

# Modules to document, in the order they appear in the reference index. A module
# not listed here is skipped, so adding bindings does not silently add a page.
MODULE_ORDER = [
    ("orca", "The plugin registration module"),
    ("orca.host", "Host application API"),
    ("orca.host.project", "Owned project snapshots"),
    ("orca.host.errors", "Shared exceptions"),
    ("orca.host.ui", "Native dialogs and plugin windows"),
    ("orca.host.plugin", "The current plugin's own storage"),
    ("orca.slicing", "Slicing pipeline hooks"),
    ("orca.script", "Script capability base class"),
    ("orca.pages", "Pages capability base class"),
    ("orca.printer_agent", "Printer agent capability base class"),
]

# Human titles for the generated pages, keyed by module name.
TITLES = {
    "orca": "Plugin Registration (orca)",
    "orca.host": "Host Application (orca.host)",
    "orca.host.project": "Project Inspection (orca.host.project)",
    "orca.host.errors": "Exceptions (orca.host.errors)",
    "orca.host.ui": "User Interface (orca.host.ui)",
    "orca.host.plugin": "Plugin Storage (orca.host.plugin)",
    "orca.slicing": "Slicing Hooks (orca.slicing)",
    "orca.script": "Script Capability (orca.script)",
    "orca.pages": "Pages Capability (orca.pages)",
    "orca.printer_agent": "Printer Agent Capability (orca.printer_agent)",
}

# pybind11 renders a py::object parameter as the bare name "object", which says
# nothing useful and collides with members that are themselves called "object"
# (Model.object, Instance.object), making the cross-reference ambiguous.
# typing.Any is what py::object actually means.
ANY_ANNOTATION = "typing.Any"

# Where the accepted types are part of the documented contract but pybind cannot
# express them, name them here rather than shipping a meaningless "object".
# Keyed by the fully qualified callable, then by parameter name.
ANNOTATION_OVERRIDES = {
    "orca.host.project.read": {"path": "str | os.PathLike[str]"},
}

OVERLOAD_HEADER = "Overloaded function."
# pybind11 opens a docstring with "name(args) -> result"; a leading "1. " marks
# one entry of an overload set.
SIGNATURE_RE = re.compile(r"^(?:\d+\.\s+)?(\w+)\((.*)\)(?:\s*->\s*(.+))?$")


def indent(text: str, prefix: str) -> str:
    return "\n".join(prefix + line if line.strip() else "" for line in text.splitlines())


def emit_directive(out: list[str], directive: str, signatures: list[str], body: str, depth: str) -> None:
    """Write one directive, putting overloads on continuation lines.

    Sphinx wants an overload set under a single directive with its extra
    signatures aligned beneath the first. Repeating the directive instead
    registers the same object twice and warns about a duplicate description.
    """
    opening = f"{depth}.. {directive}:: "
    out.append(opening + signatures[0])
    for extra in signatures[1:]:
        out.append(" " * len(opening) + extra)
    out.append("")
    out.append(indent(body or "Undocumented.", depth + "   "))
    out.append("")


def unwrap(member):
    """(callable, directive) for a plain function, staticmethod or classmethod."""
    if isinstance(member, staticmethod):
        return member.__func__, "staticmethod"
    if isinstance(member, classmethod):
        return member.__func__, "classmethod"
    return member, "method"


def shorten_types(text: str, module: str) -> str:
    """Drop the module prefix from types defined in the module being rendered.

    ``orca.host.project.Project`` reads better as ``Project`` on the page that
    defines it, while a type from elsewhere keeps its qualified name.
    """
    return text.replace(module + ".", "")


def annotate(part: str, overrides: dict[str, str]) -> str:
    """Replace an unhelpful ``object`` annotation on one parameter."""
    if ":" not in part:
        return part
    name, _, rest = part.partition(":")
    name = name.strip()
    annotation, equals, default = rest.partition("=")
    if annotation.strip() != "object":
        return part
    replacement = overrides.get(name, ANY_ANNOTATION)
    return f"{name}: {replacement}" + (f" ={default}" if equals else "")


def clean_signature(signature: str, module: str, qualified_name: str) -> str:
    """Turn a pybind11 argument list into one Sphinx can render.

    Drops the leading ``self`` and the ``/``-style positional marker pybind emits,
    replaces meaningless ``object`` annotations, and shortens types local to this
    module.
    """
    overrides = ANNOTATION_OVERRIDES.get(qualified_name, {})
    parts = split_arguments(signature)
    kept = [annotate(part, overrides) for part in parts
            if not part.startswith("self:") and part != "self" and part != "/"]
    return shorten_types(", ".join(kept), module)


def split_arguments(signature: str) -> list[str]:
    """Split an argument list on commas that are not nested in brackets."""
    parts: list[str] = []
    depth = 0
    current = ""
    for char in signature:
        if char in "([{":
            depth += 1
        elif char in ")]}":
            depth -= 1
        if char == "," and depth == 0:
            parts.append(current.strip())
            current = ""
        else:
            current += char
    if current.strip():
        parts.append(current.strip())
    return parts


def parse_doc(doc: str | None, module: str, owner: str = "") -> tuple[list[str], str]:
    """Split a pybind11 docstring into rendered signatures and a description.

    Returns ``([] , description)`` for a plain docstring, which is what
    properties and hand-written class docs have.
    """
    if not doc:
        return [], ""

    lines = doc.splitlines()
    signatures: list[str] = []
    body_start = 0

    if lines and OVERLOAD_HEADER in doc:
        # Overload sets repeat "N. name(...) -> R" with their own indented body.
        for index, line in enumerate(lines):
            match = SIGNATURE_RE.match(line.strip())
            if match and line.strip()[0].isdigit():
                signatures.append(render_signature(match, module, owner))
            elif signatures and line.strip() and not line.startswith(" "):
                break
        body = [line for line in lines if not SIGNATURE_RE.match(line.strip())]
        description = "\n".join(body).replace(OVERLOAD_HEADER, "").strip()
        return signatures, description

    match = SIGNATURE_RE.match(lines[0].strip()) if lines else None
    if match:
        signatures.append(render_signature(match, module, owner))
        body_start = 1

    description = "\n".join(lines[body_start:]).strip()
    return signatures, description


def render_signature(match: re.Match, module: str, owner: str = "") -> str:
    name, arguments, returns = match.group(1), match.group(2), match.group(3)
    qualified_name = ".".join(part for part in (module, owner, name) if part)
    rendered = f"{name}({clean_signature(arguments, module, qualified_name)})"
    if returns:
        returned = returns.strip()
        if returned == "object":
            returned = ANY_ANNOTATION
        rendered += f" -> {shorten_types(returned, module)}"
    return rendered


def is_documentable(name: str) -> bool:
    return not name.startswith("_") and not keyword.iskeyword(name)


def enum_values(cls) -> list[tuple[str, int]]:
    """(name, value) for a pybind11 enum, or an empty list for other classes.

    Enum members share the enum's own docstring, so there is nothing per-value to
    read; the name and its integer value are the whole story.
    """
    members = getattr(cls, "__members__", None)
    if not isinstance(members, dict):
        return []
    out = []
    for name, value in members.items():
        try:
            out.append((name, int(value)))
        except (TypeError, ValueError):
            out.append((name, -1))
    return out


def strip_member_listing(description: str) -> str:
    """Remove pybind's "Members:" dump, which we render as real entries instead."""
    index = description.find("Members:")
    return description[:index].strip() if index != -1 else description


def enum_member_of(value) -> tuple[str, str] | None:
    """(enum class name, member name) when `value` is a pybind enum member."""
    cls = type(value)
    if not isinstance(getattr(cls, "__members__", None), dict):
        return None
    for name, member in cls.__members__.items():
        if member == value:
            return cls.__name__, name
    return None


def write_class(out: list[str], cls, module: str) -> None:
    name = cls.__name__
    _, description = parse_doc(cls.__doc__, module)
    description = strip_member_listing(description)

    bases = [base.__name__ for base in cls.__bases__
             if base is not object and base.__name__ not in {"pybind11_object", "pybind11_type"}]
    header = f".. class:: {name}"
    if bases:
        header = f".. class:: {name}({', '.join(bases)})"
    out.append(header)
    out.append("")
    if description:
        out.append(indent(description, "   "))
        out.append("")

    values = enum_values(cls)
    if values:
        for value_name, value_number in values:
            out.append(f"   .. attribute:: {value_name}")
            out.append("")
            out.append(f"      Value ``{value_number}``.")
            out.append("")
        return

    attributes: list[tuple[str, str]] = []
    methods: list[tuple[str, str, list[str], str]] = []
    for member_name in sorted(dir(cls)):
        if not is_documentable(member_name):
            continue
        member = inspect.getattr_static(cls, member_name, None)
        if member is None:
            continue
        if isinstance(member, property):
            _, member_doc = parse_doc(member.__doc__, module)
            attributes.append((member_name, member_doc))
            continue
        # A staticmethod/classmethod wrapper carries the builtin's docstring;
        # the function it wraps is the one that was actually documented.
        member, directive = unwrap(member)
        if not callable(member):
            continue
        signatures, member_doc = parse_doc(getattr(member, "__doc__", None), module, name)
        methods.append((member_name, directive, signatures or [f"{member_name}()"], member_doc))

    for member_name, member_doc in attributes:
        out.append(f"   .. attribute:: {member_name}")
        out.append("")
        out.append(indent(member_doc or "Undocumented.", "      "))
        out.append("")

    for member_name, directive, signatures, member_doc in methods:
        emit_directive(out, directive, signatures, member_doc, "   ")


def write_module(module: types.ModuleType, name: str, summary: str, path: Path) -> int:
    title = TITLES.get(name, name)
    out = [title, "=" * len(title), "", f".. module:: {name}", ""]

    _, module_doc = parse_doc(getattr(module, "__doc__", None), name)
    out.append(module_doc or summary)
    out.append("")

    submodules = sorted(
        child for child in dir(module)
        if is_documentable(child) and isinstance(getattr(module, child), types.ModuleType))
    if submodules:
        out.append("Submodules: " + ", ".join(f":mod:`{name}.{child}`" for child in submodules))
        out.append("")

    functions = []
    classes = []
    data = []
    for member_name in sorted(dir(module)):
        if not is_documentable(member_name):
            continue
        member = getattr(module, member_name)
        if isinstance(member, types.ModuleType):
            continue
        if inspect.isclass(member):
            # Re-exported aliases (orca.host.project re-exports its exceptions)
            # belong to the module that defines them.
            if getattr(member, "__module__", name) == name:
                classes.append(member)
            else:
                data.append((member_name, member))
        elif callable(member):
            functions.append((member_name, member))
        else:
            data.append((member_name, member))

    if data:
        out.append("Attributes")
        out.append("----------")
        out.append("")
        for member_name, member in data:
            if inspect.isclass(member):
                out.append(f".. data:: {member_name}")
                out.append("")
                out.append(f"   Alias of :exc:`{member.__module__}.{member.__name__}`.")
                out.append("")
            else:
                out.append(f".. data:: {member_name}")
                out.append("")
                enum_member = enum_member_of(member)
                if enum_member:
                    # pybind's export_values() copies enum members onto the module.
                    enum_class, value_name = enum_member
                    out.append(f"   Alias of :attr:`{enum_class}.{value_name}`.")
                else:
                    out.append(f"   Value: ``{member!r}``")
                out.append("")

    if functions:
        out.append("Functions")
        out.append("---------")
        out.append("")
        for member_name, member in functions:
            signatures, description = parse_doc(getattr(member, "__doc__", None), name)
            emit_directive(out, "function", signatures or [f"{member_name}()"], description, "")

    if classes:
        out.append("Classes")
        out.append("-------")
        out.append("")
        for cls in classes:
            write_class(out, cls, name)

    path.write_text("\n".join(out).rstrip() + "\n", encoding="utf-8")
    return len(classes) + len(functions)


def module_filename(name: str) -> str:
    return name.replace(".", "_") + ".rst"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", default="generated",
                        help="directory for the generated .rst, relative to this script")
    arguments = parser.parse_args()

    try:
        import orca  # noqa: F401  - the module under documentation
    except ImportError:
        print("this script must run inside OrcaSlicer: OrcaSlicer --script "
              "docs/python_api/sphinx_doc_gen.py", file=sys.stderr)
        return 1

    output = Path(arguments.output)
    if not output.is_absolute():
        output = Path(__file__).resolve().parent / output
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)

    written = []
    for name, summary in MODULE_ORDER:
        module = sys.modules.get(name)
        if module is None:
            try:
                module = __import__(name, fromlist=["__name__"])
            except ImportError:
                print(f"skipping {name}: not present in this build", file=sys.stderr)
                continue
        count = write_module(module, name, summary, output / module_filename(name))
        written.append((name, summary, count))

    index = ["API Reference", "=" * len("API Reference"), "",
             "Generated from the running application, so every entry below exists in the",
             "binary that produced it.", "",
             ".. toctree::", "   :maxdepth: 1", ""]
    for name, _summary, _count in written:
        index.append(f"   {module_filename(name)[:-4]}")
    index.append("")
    (output / "index.rst").write_text("\n".join(index), encoding="utf-8")

    for name, summary, count in written:
        print(f"{name:24s} {count:3d} entries  ({summary})")
    print(f"\nwrote {len(written)} modules to {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
