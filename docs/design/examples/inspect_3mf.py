"""Proposed API example; requires the future orca.host.project bindings.

Proposed CLI usage after implementation:
    OrcaSlicer --file assembly.3mf --script inspect_3mf.py -- --compact

Or call inside Orca's embedded Python environment:
    report = inspect_file("assembly.3mf")
    print(json.dumps(report, indent=2, ensure_ascii=False))

This file specifies usage. It does not implement a 3MF reader.
"""

import json


def _issues(values):
    return [
        {"code": issue.code, "message": issue.message, "entity_id": issue.entity_id}
        for issue in values
    ]


def _material_summary(summary):
    return {
        "slot_ids": [material.slot_id for material in summary.items],
        "complete": summary.complete,
        "issues": _issues(summary.issues),
    }


def _instance_record(api, instance):
    space = "plate" if instance.plate_id is not None else "world"
    coordinate_issue = None
    try:
        transform = instance.transform(space=space)
    except api.CoordinateUnavailableError as error:
        coordinate_issue = str(error)
        space = "world"
        transform = instance.transform(space=space)
    bounds = instance.bounds(space=space)

    volumes = []
    for volume in instance.object.volumes:
        assignment = instance.material_assignment(volume)
        volumes.append({
            "id": volume.id,
            "name": volume.name,
            "role": str(volume.type),
            "local_size_mm": volume.bounds.size if volume.bounds.defined else None,
            "volume_to_object_matrix": volume.transform.matrix,
            "vertex_count": volume.mesh().vertex_count(),
            "triangle_count": volume.mesh().triangle_count(),
            "material_assignment": {
                "default_slot_id": assignment.default_slot_id,
                "painted_slot_ids": assignment.painted_slot_ids,
                "is_painted": assignment.is_painted,
                "source": assignment.source,
                "complete": assignment.complete,
            },
        })

    return {
        "instance_id": instance.id,
        "object_id": instance.object.id,
        "object_name": instance.object.name,
        "plate_id": instance.plate_id,
        "printable": instance.printable,
        "coordinate_space": space,
        "coordinate_issue": coordinate_issue,
        "position_mm": transform.position_mm,
        "rotation_deg": transform.rotation_deg,
        "scale": transform.scale,
        "mirror": transform.mirror,
        "matrix": transform.matrix,
        "size_mm": bounds.size if bounds.defined else None,
        "bounds_min_mm": bounds.min if bounds.defined else None,
        "bounds_max_mm": bounds.max if bounds.defined else None,
        "materials": _material_summary(instance.materials()),
        "volumes": volumes,
    }


def inspect_file(path):
    """Read an independent project and return a JSON-serializable report."""
    import orca

    api = orca.host.project
    project = api.read(path)
    return {
        "source_path": project.source_path,
        "metadata": dict(project.metadata),
        "issues": _issues(project.issues),
        "materials": [
            {
                "slot_id": material.slot_id,
                "name": material.name,
                "preset_name": material.preset_name,
                "type": material.material_type,
                "vendor": material.vendor,
                "color": material.color,
                "density_g_cm3": material.density_g_cm3,
                "diameter_mm": material.diameter_mm,
                "kind": material.kind,
                "component_slot_ids": material.component_slot_ids,
                "field_sources": dict(material.field_sources),
            }
            for material in project.materials
        ],
        "plates": [
            {
                "id": plate.id,
                "index": plate.index,
                "name": plate.name,
                "locked": plate.locked,
                "materials": _material_summary(plate.materials()),
                "instances": [_instance_record(api, item) for item in plate.instances],
            }
            for plate in project.plates
        ],
        "unassigned_instances": [
            _instance_record(api, item) for item in project.unassigned_instances
        ],
    }


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description="Inspect 3MF plates, geometry, and materials.")
    parser.add_argument("file", help="3MF input path supplied by the launcher or script caller")
    parser.add_argument("--compact", action="store_true", help="Print JSON without indentation")
    args = parser.parse_args()
    print(json.dumps(
        inspect_file(args.file),
        indent=None if args.compact else 2,
        ensure_ascii=False,
        allow_nan=False,
    ))
