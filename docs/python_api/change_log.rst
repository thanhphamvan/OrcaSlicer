Change Log
==========

The host API version returned by ``orca.host.api_version`` describes this
contract, not the application. An additive, backward-compatible change increments
the minor version. An incompatible change requires a new major version and a
documented migration.

Existing plugin bindings that predate this contract keep their current behaviour
and are not covered by these version numbers.

1.0
---

First version. Introduced with OrcaSlicer |app_version|.

Added
^^^^^

``orca.host``
   ``api_version`` and :func:`~orca.host.capabilities`, which report only the
   services implemented *and* usable in the current process.

``orca.host.errors``
   :exc:`~orca.host.errors.CapabilityUnavailableError` and its subclass
   :exc:`~orca.host.errors.ApplicationUnavailableError`;
   :exc:`~orca.host.errors.ProjectReadError` and its subclass
   :exc:`~orca.host.errors.UnsupportedProjectError`;
   :exc:`~orca.host.errors.CoordinateUnavailableError`.

``orca.host.project``
   :func:`~orca.host.project.read` and the read-only snapshot graph:
   :class:`~orca.host.project.Project`, :class:`~orca.host.project.Plate`,
   :class:`~orca.host.project.Object`, :class:`~orca.host.project.Volume`,
   :class:`~orca.host.project.Instance`,
   :class:`~orca.host.project.Transform`, :class:`~orca.host.project.Material`,
   :class:`~orca.host.project.MaterialAssignment`,
   :class:`~orca.host.project.MaterialSummary` and
   :class:`~orca.host.project.Issue`. The same exception names are re-exported
   here as aliases to the identical class objects.

Command line
   ``--script`` and ``--file``, with the ``--`` delimiter, the documented
   ``sys.argv`` layout, and the script-mode exit codes. See :doc:`info_cli`.

Capabilities in this version
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``project.read``
   Registered wherever the project bindings load.

``script.execute``
   Registered only while the command-line runner is executing a user script, so
   an embedded interpreter that is not running one does not advertise it.

Not in this version
^^^^^^^^^^^^^^^^^^^

The following are specified in the design but not implemented, and are absent
from the module rather than present as placeholders. Check
:func:`~orca.host.capabilities` rather than assuming a future release added them.

* ``orca.host.project.snapshot()`` — capturing the current GUI document
* ``orca.host.ops`` — opening a project and reordering plates in the GUI
* ``orca.host.devices`` — read-only printer status snapshots
* ``orca.host.analysis`` — solid mass estimation
* ``orca.host.slicing`` — slicing job submission and consumption estimates
* The in-application interactive shell

The exceptions those services raise — ``BusyError``, ``StaleSnapshotError``,
``OperationCancelledError`` and ``SlicingError`` — arrive with them.
