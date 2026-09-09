Quickstart
==========

The shortest useful script reads a project and prints what is on each plate.

Your first script
-----------------

Save this as ``inspect.py`` anywhere on disk:

.. code-block:: python

   import sys
   import orca

   project = orca.host.project.read(sys.argv[1])

   print(f"{len(project.objects)} objects, {len(project.instances)} copies")
   for plate in project.plates:
       print(f"plate {plate.index}: {len(plate.instances)} copies")
       for instance in plate.instances:
           print("   ", instance.object.name)

Run it with the model as the input file:

.. code-block:: sh

   OrcaSlicer --file assembly.3mf --script inspect.py

``--file`` puts the model's absolute path in ``sys.argv[1]``; it does not open or
parse anything. The project is only read when your script calls
:func:`orca.host.project.read`.

Passing your own arguments
--------------------------

Everything after ``--`` goes to the script untouched, so you can use
:mod:`argparse` as you would anywhere else:

.. code-block:: sh

   OrcaSlicer --file assembly.3mf --script inspect.py -- --format json --verbose

Reading geometry
----------------

Sizes and positions are millimetres. Ask for a coordinate space explicitly — the
API never picks one for you:

.. code-block:: python

   instance = project.instances[0]

   world = instance.transform(space="world")
   print("position", world.position_mm)
   print("rotation", world.rotation_deg)     # None for a sheared transform

   bounds = instance.bounds(space="world")
   if bounds.defined:                        # empty geometry has no bounds
       print("size", bounds.size)

``space="plate"`` gives the same values in the copy's own bed frame. It raises
:exc:`~orca.host.errors.CoordinateUnavailableError` when the copy is on no plate,
or when the file has no recoverable plate frame.

Reading materials
-----------------

Filament slot IDs are one-based, and a project lists every configured slot even
if nothing uses it:

.. code-block:: python

   for material in project.materials:
       print(material.slot_id, material.preset_name, material.color)

   # What a plate actually references:
   summary = project.plates[0].materials()
   print([m.slot_id for m in summary.items], summary.complete)

   # Which slot one part uses, and why:
   volume = project.instances[0].object.volumes[0]
   assignment = project.instances[0].material_assignment(volume)
   print(assignment.default_slot_id, assignment.source)

Handling missing data
---------------------

The API never invents a value. Unknown fields are ``None``, and anything it could
not represent shows up as a machine-readable issue code:

.. code-block:: python

   for issue in project.issues:
       print(issue.code, "-", issue.message)

   material = project.material(1)
   if material.density_g_cm3 is None:
       print("no density recorded for slot 1")

Producing output
----------------

Your script owns ``stdout``. OrcaSlicer's own logging goes to ``stderr``, so
piping JSON into another tool is safe:

.. code-block:: python

   import json
   print(json.dumps(report, allow_nan=False))

.. code-block:: sh

   OrcaSlicer --file assembly.3mf --script report.py | jq .plates

Where to go next
----------------

* :doc:`info_overview` — how the API is laid out and what it will and will not do
* :doc:`info_cli` — the full command line, ``sys.argv`` and exit codes
* :doc:`info_gotchas` — lifetimes, coordinate frames and other sharp edges
* :repo:`docs/design/examples/inspect_3mf.py` — a complete inspection script
