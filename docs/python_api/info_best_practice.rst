Best Practice
=============

Conventions that keep a script working across projects and releases.

Check what you need before you use it
-------------------------------------

Ask for the capability, and fail with a clear message rather than an
:exc:`AttributeError` deep in your code:

.. code-block:: python

   import orca

   required = {"project.read"}
   missing = required - orca.host.capabilities()
   if missing:
       raise SystemExit(f"this OrcaSlicer build is missing: {', '.join(sorted(missing))}")

Guard on the major version if you depend on the contract itself:

.. code-block:: python

   major, minor = orca.host.api_version
   if major != 1:
       raise SystemExit(f"this script targets host API 1.x, found {major}.{minor}")

Join on IDs, display names
--------------------------

Names are not unique and are meant for humans. Everything you build an index on
should key off IDs:

.. code-block:: python

   by_object = {}
   for instance in project.instances:
       by_object.setdefault(instance.object.id, []).append(instance)

Remember that IDs only mean something inside their own snapshot.

Be explicit about coordinate space
----------------------------------

``space`` is a required keyword for a reason: the two frames answer different
questions. Use ``"plate"`` for anything a user would describe relative to a bed,
and ``"world"`` when you need one frame across the whole project.

.. code-block:: python

   def placement(instance):
       """Plate coordinates where possible, world as a labelled fallback."""
       try:
           return "plate", instance.transform(space="plate")
       except orca.host.errors.CoordinateUnavailableError:
           return "world", instance.transform(space="world")

Do not paper over the difference by reporting one as the other.

Branch on issue codes, not messages
-----------------------------------

``code`` is stable and meant for programs; ``message`` is for people and can be
reworded in any release.

.. code-block:: python

   codes = {issue.code for issue in project.issues}
   if "plate_metadata_missing" in codes:
       print("no plate information in this file", file=sys.stderr)

Treat ``None`` as information
-----------------------------

A missing value is a fact about the project, not an error to smooth over.
Reporting a substituted default as if it were read from the file is worse than
reporting nothing:

.. code-block:: python

   # Wrong: invents data
   density = material.density_g_cm3 or 1.24

   # Right: says what is known
   if material.density_g_cm3 is None:
       report["density_g_cm3"] = None
       report["density_known"] = False

Use ``field_sources`` when it matters where a value came from.

Keep totals honest
------------------

If part of a calculation is unavailable, do not present the partial sum as a
total. Report the parts you have and mark the result incomplete — the API applies
the same rule to itself.

Let exceptions carry meaning
----------------------------

Catch the narrowest class that describes what you can actually handle:

.. code-block:: python

   for path in paths:
       try:
           projects.append(orca.host.project.read(path))
       except FileNotFoundError:
           print(f"missing: {path}", file=sys.stderr)
       except orca.host.errors.UnsupportedProjectError as error:
           print(f"unsupported: {path}: {error}", file=sys.stderr)
       except orca.host.errors.ProjectReadError as error:
           print(f"unreadable: {path}: {error}", file=sys.stderr)

:exc:`~orca.host.errors.UnsupportedProjectError` derives from
:exc:`~orca.host.errors.ProjectReadError`, so order the handlers accordingly. The
same classes are re-exported from :mod:`orca.host.project` as aliases to the
identical objects, so either import works.

Keep stdout for your output
---------------------------

Diagnostics and progress belong on ``stderr``. That is what makes a script
composable:

.. code-block:: python

   print(f"reading {path}", file=sys.stderr)     # progress
   print(json.dumps(report, allow_nan=False))    # the result

``allow_nan=False`` is worth the habit: every numeric result the API returns is
finite or ``None``, so a ``NaN`` in your output means your own arithmetic
produced it.

Exit deliberately
-----------------

Use ``SystemExit`` with a code in ``0..125`` to say what happened, and let real
errors propagate so the runner can classify them.

.. code-block:: python

   if not project.plates:
       raise SystemExit(3)      # "nothing to do" in this script's own vocabulary

Prefer real fixtures in tests
-----------------------------

When you test a script, run it against an actual project file through the real
executable. A mocked ``orca`` module proves your mock works, not your script.
