Gotchas
=======

Things that are easy to get wrong, and what the API actually promises.

Objects and instances are not the same thing
--------------------------------------------

An **object** is a unique definition. An **instance** is one placed copy. Two
copies of one object can be on different plates, so grouping object definitions
by plate gives the wrong answer.

.. code-block:: python

   # Wrong: an object is not on a plate; its copies are.
   for obj in project.objects:
       ...

   # Right:
   for plate in project.plates:
       for instance in plate.instances:
           print(plate.index, instance.object.name)

Names are for display, IDs are for joins — names need not be unique.

Entity IDs are snapshot-local
-----------------------------

IDs are unique inside one snapshot and stable for its lifetime. They are not
pointer addresses, not sequence indexes, and not persistent across reads. Two
snapshots of the same file will not agree on them, so never use an ID to join
one snapshot to another.

Not every copy is on a plate
----------------------------

An archive without plate metadata yields ``plates == ()`` with every copy in
``unassigned_instances`` and a ``plate_metadata_missing`` issue. The API will not
invent a bed or guess membership from proximity.

.. code-block:: python

   assigned = sum(len(plate.instances) for plate in project.plates)
   assert assigned + len(project.unassigned_instances) == len(project.instances)

``printable`` is independent of membership, and does not certify bed containment,
successful validation, or that slicing results exist.

Plate coordinates can be unavailable
------------------------------------

``space="plate"`` needs two things: the copy must belong to a plate, and that
plate must have a recoverable frame. Missing either raises
:exc:`~orca.host.errors.CoordinateUnavailableError`. ``space="world"`` always
works.

.. code-block:: python

   try:
       transform = instance.transform(space="plate")
   except orca.host.errors.CoordinateUnavailableError:
       transform = instance.transform(space="world")

Plate axes and origin follow the native bed coordinates. Do not assume the origin
is the bed centre, or its lower-left corner, or that plates are evenly spaced
along one axis — they are laid out on a grid that wraps into a second row.

``position_mm`` is not the bounds minimum
-----------------------------------------

``position_mm`` is the transformed object origin. It may sit outside the bounds
entirely. ``bounds(space=...).size`` is the axis-aligned envelope **after**
rotation, scale and mirroring — not an oriented box, and not the raw mesh size.

Always check ``defined`` before reading bounds values; empty geometry has none:

.. code-block:: python

   bounds = instance.bounds(space="world")
   size = bounds.size if bounds.defined else None

Modifiers and negative volumes do not enlarge bounds. Bounds describe the
positive-part envelope, not a boolean-subtracted final solid.

A decomposed transform can be absent
------------------------------------

For a sheared or singular transform, ``rotation_deg``, ``scale`` and ``mirror``
are ``None`` *together*. The matrix and position remain available.

.. code-block:: python

   if transform.rotation_deg is None:
       apply(transform.matrix)          # always available
   else:
       apply_trs(transform.rotation_deg, transform.scale, transform.mirror)

Euler decompositions are not unique. The convention is
``M = T @ Rz @ Ry @ Rx @ scale @ mirror`` with degrees in ``(x, y, z)`` order, but
validate the reconstructed matrix rather than expecting a particular branch.

The degree-valued ``rotation_deg`` here is a different property from the existing
raw binding's radian-valued ``rotation()`` on :class:`orca.host.ModelInstance`.

Config mappings do not inherit
------------------------------

Each ``config`` mapping holds the overrides set *at that scope*. A missing key
means "not overridden here", not "not set anywhere".

.. code-block:: python

   obj.config.get("layer_height")     # this object's override, or None
   project.config["layer_height"]     # the project value

Resolving an effective value across scopes is not part of this version.

Material inventories are references, not consumption
----------------------------------------------------

``materials()`` is a conservative inventory of the slots some geometry
*references*. It is not proof that every slot will be extruded, and it says
nothing about grams. Supports, purge and consumed material belong to slicing
statistics, which this version does not provide.

``complete`` describes reference resolution only — a summary can be ``complete``
while individual materials still have unknown density or colour.

A filament slot is not a nozzle
-------------------------------

Slot IDs are one-based logical filament slots. A slot is not an AMS bay, a
physical nozzle, or an extruder, and the numbering need not be contiguous.

.. code-block:: python

   project.material(1)      # slot 1
   project.materials[0]     # the first material in slot-ID order — may be slot 3

A ``mixed`` slot is virtual: it keeps its own ID and lists the physical slots it
blends in ``component_slot_ids``. Follow those explicitly; no mixing ratio is
implied and no density is computed for you.

Unknown means ``None``, not a default
-------------------------------------

Missing density, colour or vendor is reported as ``None``. No plausible default
is substituted, and an invalid value becomes ``None`` plus an issue rather than a
number you might trust.

.. code-block:: python

   for issue in project.issues:
       print(issue.code)      # branch on the code; the message may change

``field_sources`` records where each typed field came from, so you can tell a
value read from the project apart from one that came from an embedded preset.

Lifetimes are safe, but read them once
--------------------------------------

Child wrappers keep the snapshot alive, so an instance, volume or mesh stays
valid after ``del project``, after another read, and after the source file is
removed. What you cannot do is compare entities across two snapshots.

Meshes are immutable snapshots shared between threads. Do not attempt to mutate
one obtained from the graph.

NumPy is optional, mostly
-------------------------

Scalar reads — bounds, transforms, material fields, vertex and triangle counts,
and indexed ``vertex()``/``triangle()`` access — need no NumPy. The bulk array
accessors on meshes and polygons do, and raise :exc:`ImportError` with an
actionable message when it is missing.

There is no sandbox
-------------------

A user script runs with the privileges of the OrcaSlicer process. The plugin
audit system still applies to plugins, but running arbitrary Python is not
sandboxed and is not presented as such. Do not run scripts you would not run
directly.

Ctrl+C may not be instant
-------------------------

Cancellation is cooperative. A native call already in progress runs to
completion before :exc:`KeyboardInterrupt` is delivered, so cancelling during a
long native operation can take a while.
