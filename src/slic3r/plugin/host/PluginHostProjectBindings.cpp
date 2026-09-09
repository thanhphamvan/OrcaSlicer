#include "PluginHostApi.hpp"
#include "PluginHostBindings.hpp"
#include "PluginHostMesh.hpp"
#include "PluginHostProject.hpp"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <memory>
#include <string>
#include <vector>

namespace py = pybind11;

namespace Slic3r {
namespace {

using host_project::ProjectHandle;
using host_project::ProjectSnapshot;
using host_project::ProjectSnapshotPtr;

// Every wrapper below is a value type holding a shared_ptr to the snapshot plus
// an index into it. That keeps the owned native state alive for as long as any
// child is reachable from Python, without creating a reference cycle back into
// the interpreter, and makes `del project` harmless for retained children.
// Project itself uses host_project::ProjectHandle so native code can reach the
// snapshot behind a Python object it already holds.
struct PyPlate
{
    ProjectSnapshotPtr snapshot;
    size_t             index = 0;
};
struct PyObjectDef
{
    ProjectSnapshotPtr snapshot;
    size_t             index = 0;
};
struct PyVolume
{
    ProjectSnapshotPtr snapshot;
    size_t             index = 0;
};
struct PyInstance
{
    ProjectSnapshotPtr snapshot;
    size_t             index = 0;
};
struct PyTransform
{
    host_project::TransformValue value;
};
struct PyMaterial
{
    host_project::MaterialRecord record;
};
struct PyMaterialAssignment
{
    host_project::MaterialAssignmentRecord record;
};
struct PyMaterialSummary
{
    ProjectSnapshotPtr                  snapshot;
    host_project::MaterialSummaryRecord record;
};
struct PyIssue
{
    host_project::Issue issue;
};

py::object read_only_mapping(const std::map<std::string, std::string>& values)
{
    py::dict mapping;
    for (const auto& entry : values)
        mapping[py::str(entry.first)] = py::str(entry.second);
    return py::module_::import("types").attr("MappingProxyType")(mapping);
}

py::tuple vec3(const Vec3d& value) { return py::make_tuple(value.x(), value.y(), value.z()); }

py::object optional_string(const std::optional<std::string>& value)
{
    return value ? py::object(py::cast(*value)) : py::object(py::none());
}

py::object optional_double(const std::optional<double>& value)
{
    return value ? py::object(py::cast(*value)) : py::object(py::none());
}

py::tuple issue_tuple(const std::vector<host_project::Issue>& issues)
{
    py::list out;
    for (const host_project::Issue& issue : issues)
        out.append(py::cast(PyIssue{issue}));
    return py::tuple(out);
}

py::tuple instance_tuple(const ProjectSnapshotPtr& snapshot, const std::vector<size_t>& indices)
{
    py::list out;
    for (size_t index : indices)
        out.append(py::cast(PyInstance{snapshot, index}));
    return py::tuple(out);
}

// True for "world", false for "plate". Any other value is a caller error: the
// coordinate system is never chosen silently.
bool is_world_space(const std::string& space)
{
    if (space == "world")
        return true;
    if (space == "plate")
        return false;
    throw py::value_error("space must be \"world\" or \"plate\", got \"" + space + "\"");
}

std::map<std::string, std::string> serialize(const ConfigBase& config)
{
    std::map<std::string, std::string> out;
    for (const std::string& key : config.keys())
        out.emplace(key, config.opt_serialize(key));
    return out;
}

} // namespace

void host_bindings::register_project(py::module_& host)
{
    auto project = host.def_submodule("project",
        "Owned, read-only snapshots of a project.\n"
        "read() loads a file with Orca's own importers and retains the resulting native\n"
        "project; the views below reference that storage. Distances are millimetres,\n"
        "material slot IDs are one-based, plate indexes are zero-based, and every\n"
        "sequence is a tuple while every mapping rejects mutation. A retained child\n"
        "stays valid after the Project is released or another file is read.");

    // The project namespace raises these; re-export the identical class objects
    // rather than a second hierarchy, so `except` works against either name.
    for (const char* name : {"ProjectReadError", "UnsupportedProjectError", "CoordinateUnavailableError",
                             "CapabilityUnavailableError", "ApplicationUnavailableError"})
        project.attr(name) = host_api::error_class(name);

    py::class_<PyIssue>(project, "Issue",
        "Something the reader could not represent fully. Scripts branch on `code`;\n"
        "`message` is explanatory and may change between releases.")
        .def_property_readonly("code", [](const PyIssue& self) { return self.issue.code; },
            "Stable machine-readable identifier for this kind of issue. Branch on this, not on the message.\n\n:type: str")
        .def_property_readonly("message", [](const PyIssue& self) { return self.issue.message; },
            "Human-readable explanation. May change between releases.\n\n:type: str")
        .def_property_readonly("entity_id", [](const PyIssue& self) -> py::object {
            return self.issue.entity_id.empty() ? py::none() : py::cast(self.issue.entity_id);
        },
            "Snapshot ID of the entity this issue is about, or None when it applies to the project.\n\n:type: str | None")
        .def("__repr__", [](const PyIssue& self) { return "<orca.host.project.Issue " + self.issue.code + ">"; });

    py::class_<PyTransform>(project, "Transform",
        "An affine transform in millimetres, acting on column vectors (x, y, z, 1).\n"
        "`matrix` is the authoritative 4x4 as row tuples, with translation in the last\n"
        "column. When the transform decomposes, it follows M = T @ Rz @ Ry @ Rx @ scale\n"
        "@ mirror with degree-valued angles; for shear or singular transforms\n"
        "rotation_deg, scale and mirror are None together.")
        .def_property_readonly("matrix", [](const PyTransform& self) {
            py::list rows;
            for (int row = 0; row < 4; ++row)
                rows.append(py::make_tuple(self.value.matrix.matrix()(row, 0), self.value.matrix.matrix()(row, 1),
                                           self.value.matrix.matrix()(row, 2), self.value.matrix.matrix()(row, 3)));
            return py::tuple(rows);
        },
            "The 4x4 affine matrix as four row tuples, acting on column vectors ``(x, y, z, 1)``,\nwith translation in the last column. This is the authoritative form; the decomposed\nproperties below are a convenience.\n\n:type: tuple[tuple[float, ...], ...]")
        .def_property_readonly("position_mm", [](const PyTransform& self) { return vec3(self.value.position); },
            "The transformed object origin in millimetres. This is not the bounds minimum or centre.\n\n:type: tuple[float, float, float]")
        .def_property_readonly("rotation_deg", [](const PyTransform& self) -> py::object {
            return self.value.decomposed ? py::object(vec3(self.value.rotation_deg)) : py::none();
        },
            "Euler angles in degrees, in ``(x, y, z)`` order, following\n``M = T @ Rz @ Ry @ Rx @ scale @ mirror``. None for a shear or singular transform.\nEuler decompositions are not unique, so validate the reconstructed matrix rather\nthan a particular branch.\n\n:type: tuple[float, float, float] | None")
        .def_property_readonly("scale", [](const PyTransform& self) -> py::object {
            return self.value.decomposed ? py::object(vec3(self.value.scale)) : py::none();
        },
            "Non-negative scale factors per axis, or None for a shear or singular transform.\n\n:type: tuple[float, float, float] | None")
        .def_property_readonly("mirror", [](const PyTransform& self) -> py::object {
            if (!self.value.decomposed)
                return py::none();
            return py::make_tuple(int(self.value.mirror.x()), int(self.value.mirror.y()), int(self.value.mirror.z()));
        },
            "Per-axis mirror of ``-1`` or ``+1``, or None for a shear or singular transform.\n\n:type: tuple[int, int, int] | None");

    py::class_<PyMaterial>(project, "Material",
        "One FFF filament slot of the project. A slot is not an AMS bay, a physical\n"
        "nozzle or an extruder. Unknown fields stay None instead of being defaulted;\n"
        "field_sources records where each typed field came from.")
        .def_property_readonly("slot_id", [](const PyMaterial& self) { return self.record.slot_id; },
            "One-based filament slot ID. Slots need not be contiguous, and a slot is not an\nAMS bay, a physical nozzle or an extruder.\n\n:type: int")
        .def_property_readonly("name", [](const PyMaterial& self) { return optional_string(self.record.name); },
            "Display name for this slot, or None when the project carries none.\n\n:type: str | None")
        .def_property_readonly("preset_name", [](const PyMaterial& self) { return optional_string(self.record.preset_name); },
            "Filament profile label, such as ``Generic PLA @BBL H2D``. Do not parse a brand or\npolymer out of it; use :attr:`vendor` and :attr:`material_type`.\n\n:type: str | None")
        .def_property_readonly("material_type", [](const PyMaterial& self) { return optional_string(self.record.material_type); },
            "Material type such as ``PLA`` or ``PETG``, or None when unknown.\n\n:type: str | None")
        .def_property_readonly("vendor", [](const PyMaterial& self) { return optional_string(self.record.vendor); },
            "Filament vendor, or None when unknown.\n\n:type: str | None")
        .def_property_readonly("color", [](const PyMaterial& self) { return optional_string(self.record.color); },
            "Colour normalized to ``#RRGGBB`` or ``#RRGGBBAA``, or None when absent or unrecognized.\n\n:type: str | None")
        .def_property_readonly("density_g_cm3", [](const PyMaterial& self) { return optional_double(self.record.density_g_cm3); },
            "Density in g/cm3, or None when unknown. No default is substituted.\n\n:type: float | None")
        .def_property_readonly("diameter_mm", [](const PyMaterial& self) { return optional_double(self.record.diameter_mm); },
            "Filament diameter in millimetres, or None when unknown.\n\n:type: float | None")
        .def_property_readonly("kind", [](const PyMaterial& self) { return self.record.kind; },
            "``physical`` for an ordinary slot, ``mixed`` for a virtual slot blending several\nphysical ones, or ``unknown`` for a slot referenced by geometry but never configured.\n\n:type: str")
        .def_property_readonly("component_slot_ids", [](const PyMaterial& self) {
            return py::tuple(py::cast(self.record.component_slot_ids));
        },
            "For a ``mixed`` slot, the one-based physical slots it blends. Empty otherwise.\nFollow these explicitly; no mixing ratio is implied.\n\n:type: tuple[int, ...]")
        .def_property_readonly("field_sources", [](const PyMaterial& self) { return read_only_mapping(self.record.field_sources); },
            "Where each typed field came from: ``project_config``, ``embedded_preset``,\n``slice_metadata`` or ``unavailable``. Keys are the field names on this class.\n\n:type: Mapping[str, str]")
        .def_property_readonly("config", [](const PyMaterial& self) { return read_only_mapping(self.record.config); },
            "Serialized filament settings for this slot, with vector entries already selected\nby slot. Missing keys stay missing.\n\n:type: Mapping[str, str]")
        .def("__repr__", [](const PyMaterial& self) {
            return "<orca.host.project.Material slot " + std::to_string(self.record.slot_id) + ">";
        });

    py::class_<PyMaterialAssignment>(project, "MaterialAssignment",
        "How one part of one copy resolves to filament slots. `source` is volume,\n"
        "object, project_default, unknown, or not_applicable for a non-part volume.\n"
        "`complete` describes reference resolution, not metadata completeness.")
        .def_property_readonly("default_slot_id", [](const PyMaterialAssignment& self) -> py::object {
            return self.record.default_slot_id ? py::cast(*self.record.default_slot_id) : py::none();
        },
            "The slot this part is assigned to, or None when it could not be resolved.\nNative ``extruder == 0`` inheritance is resolved, never reported as a slot.\n\n:type: int | None")
        .def_property_readonly("painted_slot_ids", [](const PyMaterialAssignment& self) {
            return py::tuple(py::cast(self.record.painted_slot_ids));
        },
            "Slots referenced by multi-material painting on this part, sorted and deduplicated.\n\n:type: tuple[int, ...]")
        .def_property_readonly("is_painted", [](const PyMaterialAssignment& self) { return self.record.is_painted; },
            "True when the part carries multi-material painting. Stays True even if the\npainted slots could not be fully decoded.\n\n:type: bool")
        .def_property_readonly("source", [](const PyMaterialAssignment& self) { return self.record.source; },
            "Where the default slot came from: ``volume``, ``object``, ``project_default``,\n``unknown``, or ``not_applicable`` for a volume that is not a model part.\n\n:type: str")
        .def_property_readonly("complete", [](const PyMaterialAssignment& self) { return self.record.complete; },
            "False when the referenced slots could not be fully resolved. It describes\nreference resolution, not whether the material's metadata is complete.\n\n:type: bool");

    py::class_<PyMaterialSummary>(project, "MaterialSummary",
        "A deduplicated inventory of the slots some geometry references, sorted by slot\n"
        "ID. It is a reference inventory, not a statement about consumed material.")
        .def_property_readonly("items", [](const PyMaterialSummary& self) {
            py::list out;
            for (int slot_id : self.record.slot_ids) {
                const host_project::MaterialRecord* record = self.snapshot->material(slot_id);
                if (record != nullptr)
                    out.append(py::cast(PyMaterial{*record}));
            }
            return py::tuple(out);
        },
            "Referenced materials, deduplicated and sorted by slot ID.\n\n:type: tuple[Material, ...]")
        .def_property_readonly("complete", [](const PyMaterialSummary& self) { return self.record.complete; },
            "False when at least one contributing assignment could not be fully resolved.\n\n:type: bool")
        .def_property_readonly("issues", [](const PyMaterialSummary& self) { return issue_tuple(self.record.issues); },
            "Issues raised while building this inventory.\n\n:type: tuple[Issue, ...]");

    py::class_<PyVolume>(project, "Volume",
        "One volume of an object definition: a part, negative volume, or modifier.\n"
        "`bounds` is in mesh-local space and `transform` maps that mesh into object\n"
        "coordinates. `config` holds this volume's own overrides, not inherited values.")
        .def_property_readonly("id", [](const PyVolume& self) { return self.snapshot->volumes()[self.index].id; },
            "Opaque ID, unique within this snapshot only. Do not join two snapshots by ID.\n\n:type: str")
        .def_property_readonly("name", [](const PyVolume& self) { return self.snapshot->volumes()[self.index].volume->name; },
            "Volume name as imported. Names need not be unique.\n\n:type: str")
        .def_property_readonly("type", [](const PyVolume& self) { return self.snapshot->volumes()[self.index].volume->type(); },
            "Role of this volume: a model part, negative volume, or modifier.\n\n:type: orca.host.ModelVolumeType")
        .def_property_readonly("config", [](const PyVolume& self) {
            return read_only_mapping(serialize(self.snapshot->volumes()[self.index].volume->config.get()));
        },
            "This volume's own serialized setting overrides. It does not fall back to the\nobject or project scope.\n\n:type: Mapping[str, str]")
        .def_property_readonly("transform", [](const PyVolume& self) {
            return PyTransform{host_project::TransformValue::from_matrix(self.snapshot->volumes()[self.index].volume->get_matrix())};
        },
            "Maps this volume's mesh into its object's coordinates.\n\n:type: Transform")
        .def_property_readonly("bounds", [](const PyVolume& self) {
            // Undefined rather than a degenerate box when the volume has no facets.
            return bbox_from_stats(self.snapshot->volumes()[self.index].volume->mesh().stats());
        },
            "Axis-aligned bounds of the mesh in its own local space, in millimetres.\nCheck ``defined`` before reading the values: empty geometry has no bounds.\n\n:type: orca.host.BoundingBox")
        .def("mesh", [](const PyVolume& self) {
            // The volume owns its mesh as a copy-on-write shared_ptr; handing out that
            // same pointer keeps the geometry alive independently of this snapshot.
            // The TriangleMesh binding exposes read-only methods only.
            return std::const_pointer_cast<TriangleMesh>(
                self.snapshot->volumes()[self.index].volume->get_mesh_shared_ptr());
        }, "Return this volume's TriangleMesh in local coordinates. Immutable.")
        .def("__repr__", [](const PyVolume& self) {
            return "<orca.host.project.Volume " + self.snapshot->volumes()[self.index].id + ">";
        });

    py::class_<PyObjectDef>(project, "Object",
        "A unique object definition, shared by all of its copies. `bounds` is the\n"
        "object-space envelope of its model parts after volume transforms, before any\n"
        "instance transform.")
        .def_property_readonly("id", [](const PyObjectDef& self) { return self.snapshot->objects()[self.index].id; },
            "Opaque ID, unique within this snapshot only.\n\n:type: str")
        .def_property_readonly("name", [](const PyObjectDef& self) { return self.snapshot->objects()[self.index].object->name; },
            "Object name as imported. Names need not be unique; join on IDs instead.\n\n:type: str")
        .def_property_readonly("config", [](const PyObjectDef& self) {
            return read_only_mapping(serialize(self.snapshot->objects()[self.index].object->config.get()));
        },
            "This object's own serialized setting overrides, without project inheritance.\n\n:type: Mapping[str, str]")
        .def_property_readonly("volumes", [](const PyObjectDef& self) {
            py::list out;
            for (size_t index : self.snapshot->objects()[self.index].volume_indices)
                out.append(py::cast(PyVolume{self.snapshot, index}));
            return py::tuple(out);
        },
            "Volumes of this definition, in import order: parts, negative volumes and modifiers.\n\n:type: tuple[Volume, ...]")
        .def_property_readonly("instances", [](const PyObjectDef& self) {
            return instance_tuple(self.snapshot, self.snapshot->objects()[self.index].instance_indices);
        },
            "Every placed copy of this definition, in import order.\n\n:type: tuple[Instance, ...]")
        .def_property_readonly("bounds", [](const PyObjectDef& self) {
            return self.snapshot->objects()[self.index].object->raw_mesh_bounding_box();
        },
            "Object-space envelope of the model parts after their volume transforms, before\nany instance transform.\n\n:type: orca.host.BoundingBox")
        .def("__repr__", [](const PyObjectDef& self) {
            return "<orca.host.project.Object " + self.snapshot->objects()[self.index].id + ">";
        });

    py::class_<PyInstance>(project, "Instance",
        "One placed copy of an object. Membership is independent of `printable`, which\n"
        "is the imported per-copy toggle and does not certify bed containment.")
        .def_property_readonly("id", [](const PyInstance& self) { return self.snapshot->instances()[self.index].id; },
            "Opaque ID, unique within this snapshot only.\n\n:type: str")
        .def_property_readonly("object", [](const PyInstance& self) {
            return PyObjectDef{self.snapshot, self.snapshot->instances()[self.index].object_index};
        },
            "The shared object definition this copy places.\n\n:type: Object")
        .def_property_readonly("plate_id", [](const PyInstance& self) -> py::object {
            const int plate = self.snapshot->instances()[self.index].plate_index;
            return plate < 0 ? py::none() : py::cast(self.snapshot->plates()[plate].id);
        },
            "ID of the plate this copy belongs to, or None when it is unassigned. Every copy\nis on exactly one plate or unassigned, never both.\n\n:type: str | None")
        .def_property_readonly("printable", [](const PyInstance& self) {
            return self.snapshot->instances()[self.index].instance->printable;
        },
            "The imported per-copy printable toggle. It does not certify bed containment,\nsuccessful validation, or that slicing results exist, and it is independent of\nplate membership.\n\n:type: bool")
        .def("transform", [](const PyInstance& self, const std::string& space) {
            return PyTransform{is_world_space(space) ? self.snapshot->instance_transform_world(self.index) :
                                                       self.snapshot->instance_transform_plate(self.index)};
        }, py::kw_only(), py::arg("space"),
           "Object-to-`space` transform. `space` is required: \"world\" is the imported\n"
           "project frame, \"plate\" the copy's own bed frame. Plate reads raise\n"
           "CoordinateUnavailableError without membership or a recoverable frame.")
        .def("bounds", [](const PyInstance& self, const std::string& space) {
            return is_world_space(space) ? self.snapshot->instance_bounds_world(self.index) :
                                          self.snapshot->instance_bounds_plate(self.index);
        }, py::kw_only(), py::arg("space"),
           "Axis-aligned envelope of this copy's model parts in `space`, after rotation,\n"
           "scale and mirroring. Modifiers and negative volumes do not enlarge it.")
        .def("material_assignment", [](const PyInstance& self, const PyVolume& volume) {
            if (volume.snapshot != self.snapshot)
                throw py::value_error("volume belongs to a different project snapshot");
            if (self.snapshot->volumes()[volume.index].object_index != self.snapshot->instances()[self.index].object_index)
                throw py::value_error("volume belongs to a different object");
            return PyMaterialAssignment{self.snapshot->assignment(self.index, volume.index)};
        }, py::arg("volume"))
        .def("materials", [](const PyInstance& self) {
            return PyMaterialSummary{self.snapshot, self.snapshot->summary({self.index})};
        }, "Deduplicated inventory of the slots this copy's parts reference.")
        .def("__repr__", [](const PyInstance& self) {
            return "<orca.host.project.Instance " + self.snapshot->instances()[self.index].id + ">";
        });

    py::class_<PyPlate>(project, "Plate",
        "One plate of the project, in project order. An empty plate is still a plate.\n"
        "`transform_to_world` maps this plate's bed frame into the imported world frame,\n"
        "or is None when no reliable frame could be recovered from the file.")
        .def_property_readonly("id", [](const PyPlate& self) { return self.snapshot->plates()[self.index].id; },
            "Opaque ID, unique within this snapshot only.\n\n:type: str")
        .def_property_readonly("index", [](const PyPlate& self) { return self.snapshot->plates()[self.index].index; },
            "Zero-based position of this plate in project order.\n\n:type: int")
        .def_property_readonly("name", [](const PyPlate& self) { return self.snapshot->plates()[self.index].name; },
            "Plate name as imported; empty when the project sets none.\n\n:type: str")
        .def_property_readonly("locked", [](const PyPlate& self) -> py::object {
            const std::optional<bool>& locked = self.snapshot->plates()[self.index].locked;
            return locked ? py::cast(*locked) : py::none();
        },
            "The imported lock state, or None when the project does not record one.\n\n:type: bool | None")
        .def_property_readonly("config", [](const PyPlate& self) { return read_only_mapping(self.snapshot->plates()[self.index].config); },
            "This plate's own serialized setting overrides, without project inheritance.\n\n:type: Mapping[str, str]")
        .def_property_readonly("transform_to_world", [](const PyPlate& self) -> py::object {
            const auto& frame = self.snapshot->plates()[self.index].to_world;
            return frame ? py::cast(PyTransform{*frame}) : py::none();
        },
            "Maps this plate's bed frame into the project's world frame, or None when no\nreliable frame could be recovered from the file. Plate axes and origin follow the\nnative bed coordinates; do not assume the origin is the bed centre or a corner.\n\n:type: Transform | None")
        .def_property_readonly("instances", [](const PyPlate& self) {
            return instance_tuple(self.snapshot, self.snapshot->plates()[self.index].instance_indices);
        },
            "Copies assigned to this plate, in project instance order. Empty for an empty plate.\n\n:type: tuple[Instance, ...]")
        .def("materials", [](const PyPlate& self) {
            return PyMaterialSummary{self.snapshot, self.snapshot->summary(self.snapshot->plates()[self.index].instance_indices)};
        })
        .def("__repr__", [](const PyPlate& self) {
            return "<orca.host.project.Plate " + self.snapshot->plates()[self.index].id + ">";
        });

    py::class_<ProjectHandle>(project, "Project",
        "An owned project snapshot. It never writes its source, changes the active\n"
        "project or persists configuration. Entity IDs are unique inside this snapshot\n"
        "only; do not join two snapshots by ID.")
        .def_property_readonly("source_path", [](const ProjectHandle& self) { return self.snapshot->source_path(); },
            "Absolute path this snapshot was read from.\n\n:type: str | None")
        .def_property_readonly("source_kind", [](const ProjectHandle& self) { return self.snapshot->source_kind(); },
            "``file`` for a project read from disk.\n\n:type: str")
        // A file read has no live document behind it, so both GUI tokens are None.
        .def_property_readonly("source_document_id", [](const ProjectHandle&) { return py::none(); },
            "Identity of the GUI document a snapshot came from. Always None for a file read.\n\n:type: str | None")
        .def_property_readonly("source_revision", [](const ProjectHandle&) { return py::none(); },
            "Monotonic mutation token of the GUI document. Always None for a file read.\n\n:type: int | None")
        .def_property_readonly("metadata", [](const ProjectHandle& self) { return read_only_mapping(self.snapshot->metadata()); },
            "Project metadata as imported, such as the writing application and design info.\n\n:type: Mapping[str, str]")
        .def_property_readonly("config", [](const ProjectHandle& self) { return read_only_mapping(self.snapshot->serialized_config()); },
            "Serialized project-scope settings, one entry per set key.\n\n:type: Mapping[str, str]")
        .def_property_readonly("plates", [](const ProjectHandle& self) {
            py::list out;
            for (size_t index = 0; index < self.snapshot->plates().size(); ++index)
                out.append(py::cast(PyPlate{self.snapshot, index}));
            return py::tuple(out);
        },
            "Plates in project order, including empty ones.\n\n:type: tuple[Plate, ...]")
        .def_property_readonly("objects", [](const ProjectHandle& self) {
            py::list out;
            for (size_t index = 0; index < self.snapshot->objects().size(); ++index)
                out.append(py::cast(PyObjectDef{self.snapshot, index}));
            return py::tuple(out);
        },
            "Unique object definitions, in native import order.\n\n:type: tuple[Object, ...]")
        .def_property_readonly("instances", [](const ProjectHandle& self) {
            std::vector<size_t> all(self.snapshot->instances().size());
            for (size_t index = 0; index < all.size(); ++index)
                all[index] = index;
            return instance_tuple(self.snapshot, all);
        },
            "Every placed copy, including unassigned and non-printable ones, in import order.\n\n:type: tuple[Instance, ...]")
        .def_property_readonly("unassigned_instances", [](const ProjectHandle& self) {
            return instance_tuple(self.snapshot, self.snapshot->unassigned_instances());
        },
            "Copies that belong to no plate. A copy appears here or on exactly one plate,\nnever both.\n\n:type: tuple[Instance, ...]")
        .def_property_readonly("materials", [](const ProjectHandle& self) {
            py::list out;
            for (const host_project::MaterialRecord& record : self.snapshot->materials())
                out.append(py::cast(PyMaterial{record}));
            return py::tuple(out);
        },
            "All known filament slots in slot-ID order, including configured but unused ones.\n\n:type: tuple[Material, ...]")
        .def_property_readonly("issues", [](const ProjectHandle& self) { return issue_tuple(self.snapshot->issues()); },
            "Everything the reader could not represent fully while loading this project.\n\n:type: tuple[Issue, ...]")
        .def("material", [](const ProjectHandle& self, int slot_id) {
            const host_project::MaterialRecord* record = self.snapshot->material(slot_id);
            if (record == nullptr)
                throw py::key_error("no filament slot " + std::to_string(slot_id) + " in this project");
            return PyMaterial{*record};
        }, py::arg("slot_id"), "Return the one-based filament slot `slot_id`. Raises KeyError if unknown.")
        .def("__repr__", [](const ProjectHandle& self) {
            return "<orca.host.project.Project " + self.snapshot->source_path() + ">";
        });

    project.def(
        "read",
        [](const py::object& path) {
            // Accept str and os.PathLike, the same way open() does.
            const std::string native = py::module_::import("os").attr("fspath")(path).cast<std::string>();
            py::gil_scoped_release release;
            return ProjectHandle{ProjectSnapshot::read_file(native)};
        },
        py::arg("path"),
        "Read a project file and return an owned, read-only snapshot.\n"
        "Uses the application's own format detection and importers, and the file's own\n"
        "configuration and embedded presets; it never falls back to the GUI's selected\n"
        "profiles, opens the file in the GUI, arranges geometry or slices anything.\n"
        "The file is closed before this returns.\n"
        "Raises FileNotFoundError/PermissionError for I/O problems, ProjectReadError for\n"
        "a project that cannot be read, and UnsupportedProjectError for one this version\n"
        "cannot honour.");

    host_api::enable_capability(host_api::CAP_PROJECT_READ);
}

} // namespace Slic3r
