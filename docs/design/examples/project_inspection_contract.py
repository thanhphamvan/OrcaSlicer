"""Draft acceptance cases, not registered or passing production tests.

The API and named fixtures must be implemented before these cases can run.
Fixture specifications are in ../python-project-inspection-api.md (relative to
this directory). Future use from the embedded test interpreter:

    run_contract(orca.host.project, fixture_dir)

Use the existing Catch2/embedded-Python harness when integrating these cases.
No mock reader or separate Python test-framework dependency is provided here.
"""

from contextlib import contextmanager
import gc
from math import cos, isclose, radians, sin
from pathlib import Path


@contextmanager
def _raises(exception_type):
    try:
        yield
    except exception_type:
        return
    raise AssertionError(f"Expected {exception_type.__name__}")


def _close_vector(actual, expected):
    assert len(actual) == len(expected)
    for a, b in zip(actual, expected):
        assert isclose(a, b, rel_tol=1e-9, abs_tol=1e-6), (actual, expected)


def _point(matrix, point):
    vector = (*point, 1.0)
    return tuple(sum(row[j] * vector[j] for j in range(4)) for row in matrix[:3])


def _decomposed_point(transform, point):
    """Independent check of the documented T @ Rz @ Ry @ Rx @ S @ mirror."""
    assert transform.rotation_deg is not None
    assert transform.scale is not None
    assert transform.mirror is not None
    x, y, z = (point[i] * transform.scale[i] * transform.mirror[i] for i in range(3))
    rx, ry, rz = map(radians, transform.rotation_deg)
    y, z = cos(rx) * y - sin(rx) * z, sin(rx) * y + cos(rx) * z
    x, z = cos(ry) * x + sin(ry) * z, -sin(ry) * x + cos(ry) * z
    x, y = cos(rz) * x - sin(rz) * y, sin(rz) * x + cos(rz) * y
    return tuple(value + offset for value, offset in zip((x, y, z), transform.position_mm))


def test_instances_group_by_plate_without_duplicating_object_definitions(api, fixtures):
    project = api.read(fixtures / "plates_and_materials.3mf")
    assert [plate.name for plate in project.plates] == ["Left", "Right", "Empty"]
    assert [plate.index for plate in project.plates] == [0, 1, 2]
    assert [len(plate.instances) for plate in project.plates] == [2, 1, 0]
    assert len(project.objects) == 2
    assert len(project.instances) == 3
    assert project.unassigned_instances == ()

    cube = next(obj for obj in project.objects if obj.name == "Cube")
    assert len(cube.instances) == 2
    assert len({item.object.id for item in cube.instances}) == 1
    assert len({item.id for item in cube.instances}) == 2
    assert {item.plate_id for item in cube.instances} == {
        project.plates[0].id, project.plates[1].id,
    }
    assigned = [item.id for plate in project.plates for item in plate.instances]
    assert len(assigned) == len(set(assigned))
    assert set(assigned) == {item.id for item in project.instances}


def test_rotation_changes_bounds_and_position_is_not_bounds_minimum(api, fixtures):
    project = api.read(fixtures / "plates_and_materials.3mf")
    cube = next(item for item in project.plates[0].instances if item.object.name == "Cube")
    transform = cube.transform(space="plate")
    bounds = cube.bounds(space="plate")
    assert bounds.defined
    _close_vector(cube.object.bounds.size, (10, 20, 30))
    _close_vector(transform.position_mm, (50, 60, 15))
    _close_vector(bounds.size, (20, 10, 30))
    _close_vector(bounds.min, (40, 55, 0))
    _close_vector(bounds.max, (60, 65, 30))
    _close_vector(_point(transform.matrix, (1, 2, 3)), (48, 61, 18))
    _close_vector(_decomposed_point(transform, (1, 2, 3)), (48, 61, 18))


def test_plate_and_world_coordinates_preserve_scaling_and_mirroring(api, fixtures):
    project = api.read(fixtures / "plates_and_materials.3mf")
    plate = project.plates[1]
    cube = plate.instances[0]
    local = cube.transform(space="plate")
    world = cube.transform(space="world")
    assert plate.transform_to_world is not None
    _close_vector(local.position_mm, (20, 30, 7.5))
    _close_vector(world.position_mm, (320, 30, 7.5))
    _close_vector(cube.bounds(space="plate").size, (20, 20, 15))
    _close_vector(_point(world.matrix, (1, 2, 3)), (318, 32, 9))
    _close_vector(_decomposed_point(world, (1, 2, 3)), (318, 32, 9))
    _close_vector(
        _point(plate.transform_to_world.matrix, _point(local.matrix, (1, 2, 3))),
        _point(world.matrix, (1, 2, 3)),
    )
    assert cube.printable is False  # Inspection must not silently drop this copy.


def test_material_inventory_excludes_unused_slots_and_modifier_overrides(api, fixtures):
    project = api.read(fixtures / "plates_and_materials.3mf")
    assert [material.slot_id for material in project.materials] == [1, 2, 3, 4]
    assert [material.slot_id for material in project.plates[0].materials().items] == [1, 2, 3]
    assert [material.slot_id for material in project.plates[1].materials().items] == [1]
    assert project.plates[0].materials().complete
    assert project.plates[2].materials().items == ()
    assert project.plates[2].materials().complete
    assert project.material(1).material_type == "PLA"
    assert isclose(project.material(1).density_g_cm3, 1.24, abs_tol=1e-6)
    assert project.material(2).material_type == "PETG"


def test_material_assignment_resolves_object_inheritance_and_volume_override(api, fixtures):
    project = api.read(fixtures / "plates_and_materials.3mf")
    bracket = next(item for item in project.plates[0].instances if item.object.name == "Bracket")
    volumes = {volume.name: volume for volume in bracket.object.volumes}
    body = bracket.material_assignment(volumes["Body"])
    cap = bracket.material_assignment(volumes["Cap"])
    modifier = bracket.material_assignment(volumes["Modifier"])
    assert body.default_slot_id == 2
    assert body.source == "object"
    assert cap.default_slot_id == 3
    assert cap.source == "volume"
    assert modifier.source == "not_applicable"
    assert modifier.default_slot_id is None
    assert modifier.painted_slot_ids == ()
    assert body.complete
    assert cap.complete
    assert body.is_painted is False


def test_scoped_configuration_does_not_silently_return_inherited_values(api, fixtures):
    project = api.read(fixtures / "plates_and_materials.3mf")
    cube = next(obj for obj in project.objects if obj.name == "Cube")
    assert project.config["layer_height"] == "0.2"
    assert project.plates[0].config["layer_height"] == "0.16"
    assert cube.config.get("layer_height") is None


def test_generic_geometry_remains_unassigned_without_fabricated_materials(api, fixtures):
    project = api.read(fixtures / "generic_geometry.3mf")
    assert project.plates == ()
    assert project.materials == ()
    assert len(project.instances) == 1
    instance = project.instances[0]
    assert [item.id for item in project.unassigned_instances] == [instance.id]
    assert instance.plate_id is None
    assert "plate_metadata_missing" in {issue.code for issue in project.issues}
    assert instance.bounds(space="world").defined
    assert instance.materials().complete is False
    with _raises(api.CoordinateUnavailableError):
        instance.transform(space="plate")


def test_missing_density_stays_unknown(api, fixtures):
    project = api.read(fixtures / "missing_density.3mf")
    material = project.material(1)
    assert material.material_type is not None
    assert material.density_g_cm3 is None
    assert material.field_sources["density_g_cm3"] == "unavailable"


def test_painted_material_inventory_reports_incomplete_resolution(api, fixtures):
    project = api.read(fixtures / "painted_incomplete.3mf")
    instance = project.instances[0]
    assignment = instance.material_assignment(instance.object.volumes[0])
    assert assignment.default_slot_id == 1
    assert assignment.is_painted
    assert assignment.complete is False
    summary = instance.materials()
    assert summary.complete is False
    assert "material_assignment_incomplete" in {issue.code for issue in summary.issues}


def test_shear_preserves_the_matrix_without_an_inaccurate_euler_decomposition(api, fixtures):
    project = api.read(fixtures / "sheared_instance.3mf")
    transform = project.instances[0].transform(space="world")
    assert transform.rotation_deg is None
    assert transform.scale is None
    assert transform.mirror is None
    # Translation cancels, leaving the sheared Y basis vector.
    origin = _point(transform.matrix, (0, 0, 0))
    y_axis = _point(transform.matrix, (0, 1, 0))
    _close_vector(tuple(b - a for a, b in zip(origin, y_axis)), (0.25, 1, 0))


def test_retained_children_survive_project_release_and_another_read(api, fixtures):
    project = api.read(fixtures / "plates_and_materials.3mf")
    instance = project.plates[1].instances[0]
    mesh = instance.object.volumes[0].mesh()
    triangles = mesh.triangle_count()
    del project
    gc.collect()
    other = api.read(fixtures / "generic_geometry.3mf")
    assert len(other.instances) == 1
    assert instance.object.name == "Cube"
    _close_vector(instance.transform(space="plate").position_mm, (20, 30, 7.5))
    assert mesh.triangle_count() == triangles
    assert triangles > 0


def test_readonly_values_and_invalid_requests_fail_predictably(api, fixtures):
    project = api.read(fixtures / "plates_and_materials.3mf")
    cube = project.plates[1].instances[0]
    bracket = next(obj for obj in project.objects if obj.name == "Bracket")
    with _raises(AttributeError):
        cube.object.name = "Changed"
    with _raises(TypeError):
        project.config["layer_height"] = "0.4"
    with _raises(KeyError):
        project.material(999)
    with _raises(ValueError):
        cube.transform(space="screen")
    with _raises(ValueError):
        cube.material_assignment(bracket.volumes[0])
    with _raises(api.ProjectReadError):
        api.read(fixtures / "corrupt.3mf")


def run_contract(api, fixture_dir):
    """Run the draft cases once real bindings and the documented fixtures exist."""
    fixtures = Path(fixture_dir)
    cases = (
        test_instances_group_by_plate_without_duplicating_object_definitions,
        test_rotation_changes_bounds_and_position_is_not_bounds_minimum,
        test_plate_and_world_coordinates_preserve_scaling_and_mirroring,
        test_material_inventory_excludes_unused_slots_and_modifier_overrides,
        test_material_assignment_resolves_object_inheritance_and_volume_override,
        test_scoped_configuration_does_not_silently_return_inherited_values,
        test_generic_geometry_remains_unassigned_without_fabricated_materials,
        test_missing_density_stays_unknown,
        test_painted_material_inventory_reports_incomplete_resolution,
        test_shear_preserves_the_matrix_without_an_inaccurate_euler_decomposition,
        test_retained_children_survive_project_release_and_another_read,
        test_readonly_values_and_invalid_requests_fail_predictably,
    )
    for case in cases:
        case(api, fixtures)
    return tuple(case.__name__ for case in cases)
