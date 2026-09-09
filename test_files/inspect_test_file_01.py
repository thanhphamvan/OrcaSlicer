"""Inspect and verify test_file_01.3mf through OrcaSlicer's Python API.

Intended invocation after the CLI runner and project API are implemented:

    OrcaSlicer --file test_files/test_file_01.3mf \
        --script test_files/inspect_test_file_01.py -- --verify

This is a RED behavior check, not proof of the implementation's loading path.
The native parity tests and architecture review are also required for Gate A.
"""

from __future__ import annotations

import argparse
import json
from math import cos, isclose, radians, sin
from pathlib import Path


def _vector(value):
    return list(value) if value is not None else None


def _transform_record(transform):
    if transform is None:
        return None
    return {
        "matrix": [list(row) for row in transform.matrix],
        "position_mm": _vector(transform.position_mm),
        "rotation_deg": _vector(transform.rotation_deg),
        "scale": _vector(transform.scale),
        "mirror": _vector(transform.mirror),
    }


def _material_record(material):
    return {
        "slot_id": material.slot_id,
        "name": material.name,
        "preset_name": material.preset_name,
        "type": material.material_type,
        "vendor": material.vendor,
        "color": material.color,
        "density_g_cm3": material.density_g_cm3,
        "diameter_mm": material.diameter_mm,
        "kind": material.kind,
        "component_slot_ids": list(material.component_slot_ids),
        "field_sources": dict(material.field_sources),
    }


def _material_summary(summary):
    return {
        "slot_ids": [material.slot_id for material in summary.items],
        "complete": summary.complete,
        "issues": [issue.code for issue in summary.issues],
    }


def _bounds_record(bounds):
    if not bounds.defined:
        return {"defined": False}
    return {
        "defined": True,
        "min_mm": _vector(bounds.min),
        "max_mm": _vector(bounds.max),
        "size_mm": _vector(bounds.size),
    }


def _instance_record(instance):
    volumes = []
    for volume in instance.object.volumes:
        assignment = instance.material_assignment(volume)
        mesh = volume.mesh()
        volumes.append(
            {
                "id": volume.id,
                "name": volume.name,
                "role": str(volume.type),
                "bounds": _bounds_record(volume.bounds),
                "transform": _transform_record(volume.transform),
                "vertex_count": mesh.vertex_count(),
                "triangle_count": mesh.triangle_count(),
                "material_assignment": {
                    "default_slot_id": assignment.default_slot_id,
                    "painted_slot_ids": list(assignment.painted_slot_ids),
                    "is_painted": assignment.is_painted,
                    "source": assignment.source,
                    "complete": assignment.complete,
                },
            }
        )

    plate_transform = (
        _transform_record(instance.transform(space="plate"))
        if instance.plate_id is not None
        else None
    )
    plate_bounds = (
        _bounds_record(instance.bounds(space="plate"))
        if instance.plate_id is not None
        else None
    )
    return {
        "id": instance.id,
        "object_id": instance.object.id,
        "object_name": instance.object.name,
        "plate_id": instance.plate_id,
        "printable": instance.printable,
        "world_transform": _transform_record(instance.transform(space="world")),
        "plate_transform": plate_transform,
        "world_bounds": _bounds_record(instance.bounds(space="world")),
        "plate_bounds": plate_bounds,
        "materials": _material_summary(instance.materials()),
        "volumes": volumes,
    }


def inspect_file(path):
    """Return a JSON-serializable report produced by OrcaSlicer's native reader."""
    import orca

    project = orca.host.project.read(path)
    return {
        "source_path": project.source_path,
        "source_kind": project.source_kind,
        "metadata": dict(project.metadata),
        "object_count": len(project.objects),
        "object_ids": [obj.id for obj in project.objects],
        "instance_count": len(project.instances),
        "instance_ids": [instance.id for instance in project.instances],
        "plate_count": len(project.plates),
        "unassigned_instance_count": len(project.unassigned_instances),
        "issues": [issue.code for issue in project.issues],
        "materials": [_material_record(material) for material in project.materials],
        "plates": [
            {
                "id": plate.id,
                "index": plate.index,
                "name": plate.name,
                "locked": plate.locked,
                "transform_to_world": _transform_record(plate.transform_to_world),
                "materials": _material_summary(plate.materials()),
                "instances": [_instance_record(instance) for instance in plate.instances],
            }
            for plate in project.plates
        ],
        "unassigned_instances": [
            _instance_record(instance) for instance in project.unassigned_instances
        ],
    }


def _assert_vector(actual, expected, *, tolerance=1e-6):
    assert actual is not None
    assert len(actual) == len(expected)
    for actual_value, expected_value in zip(actual, expected):
        assert isclose(actual_value, expected_value, rel_tol=0.0, abs_tol=tolerance), (
            actual,
            expected,
        )


def _point(matrix, point):
    return [sum(row[j] * (*point, 1.0)[j] for j in range(4)) for row in matrix[:3]]


def _check_decomposition(transform):
    # Validate the documented Euler convention without fixing an Euler branch.
    point = (2.0, 3.0, 5.0)
    x, y, z = [point[i] * transform["scale"][i] * transform["mirror"][i] for i in range(3)]
    rx, ry, rz = map(radians, transform["rotation_deg"])
    y, z = cos(rx) * y - sin(rx) * z, sin(rx) * y + cos(rx) * z
    x, z = cos(ry) * x + sin(ry) * z, -sin(ry) * x + cos(ry) * z
    x, y = cos(rz) * x - sin(rz) * y, sin(rz) * x + cos(rz) * y
    _assert_vector(
        [value + offset for value, offset in zip((x, y, z), transform["position_mm"])],
        _point(transform["matrix"], point),
    )


def verify_test_file_01(report):
    """Assert the native normalized values expected from test_file_01.3mf."""
    assert report["source_kind"] == "file"
    assert report["plate_count"] == 2
    assert report["object_count"] == 3
    assert report["instance_count"] == 3
    assert report["unassigned_instance_count"] == 0
    assert [len(plate["instances"]) for plate in report["plates"]] == [1, 2]
    assert [plate["index"] for plate in report["plates"]] == [0, 1]
    assert [plate["locked"] for plate in report["plates"]] == [False, False]
    assert len(set(report["object_ids"])) == 3
    assert len(set(report["instance_ids"])) == 3
    assert len({plate["id"] for plate in report["plates"]}) == 2
    assert report["unassigned_instances"] == []

    expected = {
        "3DBenchy.drc": {
            "plate": 0,
            "world_position": (175.0, 160.0, 24.0),
            "size": (60.001007, 31.003998, 48.0),
        },
        "OrcaCube_v2.drc": {
            "plate": 1,
            "world_position": (606.999252, 160.0, 15.0),
            "size": (30.0, 29.999992, 30.0),
        },
        "OrcaPlug_v2.drc": {
            "plate": 1,
            "world_position": (579.0, 160.000001, 4.898826),
            "size": (21.998505, 21.999252, 9.797651),
        },
    }

    seen = set()
    assigned_ids = []
    object_ids = []
    for plate in report["plates"]:
        assert plate["materials"] == {"slot_ids": [1], "complete": True, "issues": []}
        assert plate["transform_to_world"] is not None
        for instance in plate["instances"]:
            name = instance["object_name"]
            fixture = expected[name]
            seen.add(name)
            assigned_ids.append(instance["id"])
            object_ids.append(instance["object_id"])
            assert instance["plate_id"] == plate["id"]
            assert plate["index"] == fixture["plate"]
            assert instance["printable"] is True
            assert instance["materials"] == {
                "slot_ids": [1],
                "complete": True,
                "issues": [],
            }
            _assert_vector(
                instance["world_transform"]["position_mm"], fixture["world_position"],
                tolerance=1e-4,
            )
            for space in ("world", "plate"):
                transform = instance[f"{space}_transform"]
                _check_decomposition(transform)
                _assert_vector(transform["scale"], (1.0, 1.0, 1.0))
                _assert_vector(transform["mirror"], (1.0, 1.0, 1.0))
                _assert_vector(transform["position_mm"], [row[3] for row in transform["matrix"][:3]])
                for row, axis in zip(transform["matrix"][:3], ((1, 0, 0), (0, 1, 0), (0, 0, 1))):
                    _assert_vector(row[:3], axis)
                bounds = instance[f"{space}_bounds"]
                assert bounds["defined"]
                _assert_vector(bounds["size_mm"], fixture["size"], tolerance=1e-4)
                _assert_vector([b - a for a, b in zip(bounds["min_mm"], bounds["max_mm"])], bounds["size_mm"])
            # All four affine basis points must agree; no guessed plate stride.
            for point in ((0, 0, 0), (1, 0, 0), (0, 1, 0), (0, 0, 1)):
                _assert_vector(
                    _point(plate["transform_to_world"]["matrix"], _point(instance["plate_transform"]["matrix"], point)),
                    _point(instance["world_transform"]["matrix"], point),
                )
            assert len(instance["volumes"]) == 1
            part = instance["volumes"][0]
            assert part["vertex_count"] > 0
            assert part["triangle_count"] > 0
            assert part["material_assignment"] == {
                "default_slot_id": 1, "painted_slot_ids": [], "is_painted": False,
                "source": "object", "complete": True,
            }

    assert seen == set(expected)
    assert len(set(assigned_ids)) == 3
    assert set(assigned_ids) == set(report["instance_ids"])
    assert len(set(object_ids)) == 3
    assert set(object_ids) == set(report["object_ids"])

    expected_materials = [
        (1, "eSUN PLA+ @BBL H2D", "PLA", "eSUN", "#057748", 1.25, 1.75),
        (2, "Generic PETG @BBL H2D", "PETG", "Generic", "#FFF144", 1.27, 1.75),
        (3, "Bambu PETG Translucent @BBL H2D 0.4 nozzle", "PETG", "Bambu Lab", "#0ACC38", 1.25, 1.75),
        (4, "Generic PLA @BBL H2D", "PLA", "Generic", "#F72323", 1.24, 1.75),
        (5, "Generic PLA @BBL H2D", "PLA", "Generic", "#161616", 1.24, 1.75),
    ]
    assert len(report["materials"]) == len(expected_materials)
    for material, expected_material in zip(report["materials"], expected_materials):
        slot_id, preset, material_type, vendor, color, density, diameter = expected_material
        assert material["slot_id"] == slot_id
        assert material["preset_name"] == preset
        assert material["type"] == material_type
        assert material["vendor"] == vendor
        assert material["color"] == color
        assert material["kind"] == "physical"
        assert material["component_slot_ids"] == []
        for field in ("preset_name", "material_type", "vendor", "color", "density_g_cm3", "diameter_mm"):
            assert material["field_sources"][field] == "project_config"
        assert isclose(material["density_g_cm3"], density, rel_tol=0, abs_tol=1e-6)
        assert isclose(material["diameter_mm"], diameter, rel_tol=0, abs_tol=1e-6)


def main():
    parser = argparse.ArgumentParser(
        description="Read the two-plate OrcaSlicer fixture and print its project graph."
    )
    parser.add_argument("file", type=Path)
    parser.add_argument(
        "--verify",
        action="store_true",
        help="also assert the checked-in fixture's expected normalized values",
    )
    parser.add_argument("--compact", action="store_true")
    args = parser.parse_args()
    if args.verify and not __debug__:
        parser.error("--verify requires Python assertions enabled")

    report = inspect_file(args.file)
    if args.verify:
        verify_test_file_01(report)
    print(
        json.dumps(
            report,
            indent=None if args.compact else 2,
            ensure_ascii=False,
            allow_nan=False,
        )
    )


if __name__ == "__main__":
    main()
