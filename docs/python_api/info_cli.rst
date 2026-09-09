Running Scripts from the Command Line
=====================================

``--script`` selects a separate process mode: OrcaSlicer executes exactly one
Python file and exits. It implies running without a GUI, so there is no
``--headless`` flag to add.

.. code-block:: sh

   OrcaSlicer --script inspect.py
   OrcaSlicer --file assembly.3mf --script inspect.py
   OrcaSlicer assembly.3mf --script inspect.py
   OrcaSlicer --file assembly.3mf --script inspect.py -- --compact

Options
-------

.. list-table::
   :header-rows: 1
   :widths: 22 78

   * - Option
     - Meaning
   * - ``--script <path>``
     - The Python file to execute. Selects script mode. Required.
   * - ``--file <path>``
     - One model to hand to the script. Valid only together with ``--script``.
   * - ``--help``
     - Print help and exit 0 without executing anything.
   * - ``--``
     - Ends application parsing. Everything after it goes to the script.

Both ``--key value`` and ``--key=value`` work. Use ``--file=-model.3mf`` for a
path that starts with a dash. Option order relative to the input path does not
matter.

You may pass zero or one model. Repeating ``--file`` or ``--script``, giving two
positional inputs, or combining ``--file`` with a positional input is an error.
Slicing, export and transform options are rejected in script mode.

Without ``--script``, the existing command line and GUI startup behave exactly as
before, including the existing meaning of ``--`` and positional model arguments.

The delimiter
-------------

The **first** ``--`` ends application parsing. Script mode is detected only from
the tokens before it, and everything after it reaches your script unchanged —
even tokens that look like OrcaSlicer options:

.. code-block:: sh

   OrcaSlicer --script s.py -- --file --script -- --help

.. code-block:: python

   sys.argv  # ['/abs/s.py', '--file', '--script', '--', '--help']

Execution environment
---------------------

Your file is executed as ``__main__``, so the usual entry-point guard works.

.. list-table::
   :header-rows: 1
   :widths: 26 74

   * - Name
     - Value
   * - ``__name__``
     - ``"__main__"``
   * - ``__file__``
     - The absolute script path
   * - ``sys.argv``
     - ``[script_path, *input_paths, *script_args]``, with at most one input path
   * - ``sys.path[0]``
     - The script's directory, so sibling imports work
   * - Working directory
     - Unchanged. Paths are resolved against it, not by changing it.

For ``OrcaSlicer --file assembly.3mf --script inspect.py -- --compact``:

.. code-block:: python

   sys.argv == ["/abs/inspect.py", "/abs/assembly.3mf", "--compact"]

No ``project`` global is injected. Your script decides when to call
:func:`~orca.host.project.read`, and may read more than one file.

The interpreter is the one bundled with OrcaSlicer, and the shared package
directory plugins use is on ``sys.path``. The runner does not install missing
packages for you.

Streams
-------

``stdout`` belongs to your script and the libraries it imports. OrcaSlicer's
startup, progress and diagnostics go to ``stderr`` and the session log, so no
banner or log line can corrupt JSON on ``stdout``. Both streams are flushed
before the interpreter shuts down, on the error path as well as the normal one.

Opening a project never executes code embedded in that project.

Exit codes
----------

These apply only to script mode; the existing CLI's codes are unchanged.

.. list-table::
   :header-rows: 1
   :widths: 10 90

   * - Code
     - Meaning
   * - ``0``
     - Normal completion, ``--help``, or ``SystemExit(None)``
   * - ``1``
     - Uncaught Python exception other than the cases below
   * - ``2``
     - Invalid application arguments. The script was **not** executed
   * - ``3``
     - The bundled runtime or ``orca`` registration failed. Not executed
   * - ``4``
     - Preflight I/O failure, or an uncaught :exc:`OSError` or
       :exc:`~orca.host.errors.ProjectReadError`
   * - ``130``
     - Uncaught :exc:`KeyboardInterrupt`

An explicit ``SystemExit(n)`` with ``0 <= n <= 125`` is returned unchanged, and
takes precedence over the automatic mapping above. Outside that range you get
``1`` and a diagnostic, rather than platform-specific truncation.

Caught exceptions do not affect the exit code:

.. code-block:: python

   try:
       project = orca.host.project.read(path)
   except orca.host.errors.ProjectReadError as error:
       print(f"skipping {path}: {error}", file=sys.stderr)
   else:
       report(project)
   # still exits 0

Preflight
---------

Before your code runs, the script path and any model path are resolved against
the invocation directory and checked to be readable regular files. A failure
there is exit 4 and your script never starts. A file that disappears later is an
ordinary I/O error at the point you touch it.

Cancellation
------------

Ctrl+C requests cooperative cancellation and surfaces as
:exc:`KeyboardInterrupt`. A native call that is already running cannot be
interrupted mid-flight, so delivery can be delayed until it returns. This is a
real limitation, not immediate cancellation.
