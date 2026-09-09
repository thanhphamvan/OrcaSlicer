OrcaSlicer |app_version| Python API Documentation
=================================================

Welcome to the Python API reference for OrcaSlicer. The API is available in two
places: inside a **plugin**, which OrcaSlicer loads and calls back into, and from
a **script run on the command line**, which drives OrcaSlicer instead.

This document covers host API version |host_api_version|, which is versioned
separately from the application — see :doc:`change_log`.

.. code-block:: python

   import orca

   project = orca.host.project.read("assembly.3mf")

   for plate in project.plates:
       print(plate.index, plate.name)
       for instance in plate.instances:
           bounds = instance.bounds(space="plate")
           print("  ", instance.object.name, bounds.size)

.. code-block:: sh

   OrcaSlicer --file assembly.3mf --script inspect.py

Getting started
---------------

.. toctree::
   :maxdepth: 1

   info_quickstart
   info_overview
   info_cli

Using the API well
------------------

.. toctree::
   :maxdepth: 1

   info_best_practice
   info_gotchas
   change_log

API reference
-------------

Every page below is generated from a running OrcaSlicer, so it describes the
bindings that are actually in the binary rather than an intended surface.

.. toctree::
   :maxdepth: 2

   generated/index

Indices
-------

* :ref:`genindex`
* :ref:`modindex`
* :ref:`search`
