#pragma once

// Owned, read-only project snapshots behind `orca.host.project`.
//
// A snapshot owns the native state produced by Orca's own importers: the Model,
// the project DynamicPrintConfig, the imported PlateData and the presets embedded
// in the archive. The Python wrappers are views over that state; every child
// wrapper keeps a shared_ptr to the snapshot, so a retained instance, volume or
// mesh stays valid after the Python `Project` is released, after another file is
// read, and after the source file disappears.
//
// Nothing here re-implements project interpretation. read_file() calls
// Model::read_from_archive()/read_from_file(), which run the existing format
// detection and importers; the normalization below only reads the native state
// those produce. See docs/backlog/python-api-contract.md.

#include <libslic3r/BoundingBox.hpp>
#include <libslic3r/Format/bbs_3mf.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/Point.hpp>
#include <libslic3r/Preset.hpp>
#include <libslic3r/PrintConfig.hpp>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {
namespace host_project {

// A machine-readable note about something the reader could not represent fully.
// Scripts branch on `code`; `message` is explanatory and may change.
struct Issue
{
    std::string code;
    std::string message;
    std::string entity_id; // empty when the issue is not tied to one entity
};

// An affine transform plus its TRS decomposition, when one exists.
// The decomposition follows the native convention M = T * Rz * Ry * Rx * S * M.
struct TransformValue
{
    Transform3d matrix{Transform3d::Identity()};
    // False for shear or singular transforms: rotation/scale/mirror are then
    // unavailable together and only matrix/position remain meaningful.
    bool  decomposed = false;
    Vec3d position{Vec3d::Zero()};
    Vec3d rotation_deg{Vec3d::Zero()};
    Vec3d scale{Vec3d::Ones()};
    Vec3d mirror{Vec3d::Ones()};

    static TransformValue from_matrix(const Transform3d& matrix);
};

// One normalized FFF filament slot. Slot IDs are one-based and need not be
// contiguous; a slot is not an AMS bay, nozzle or extruder.
struct MaterialRecord
{
    int                                slot_id = 0;
    std::optional<std::string>         name;
    std::optional<std::string>         preset_name;
    std::optional<std::string>         material_type;
    std::optional<std::string>         vendor;
    std::optional<std::string>         color;
    std::optional<double>              density_g_cm3;
    std::optional<double>              diameter_mm;
    std::string                        kind = "unknown"; // physical | mixed | unknown
    std::vector<int>                   component_slot_ids;
    std::map<std::string, std::string> field_sources;
    std::map<std::string, std::string> config;
};

// How one volume of one instance resolves to filament slots.
struct MaterialAssignmentRecord
{
    std::optional<int> default_slot_id;
    std::vector<int>   painted_slot_ids;
    bool               is_painted = false;
    // volume | object | project_default | unknown | not_applicable
    std::string        source   = "unknown";
    bool               complete = true;
};

// A deduplicated inventory of the slots referenced by some geometry.
struct MaterialSummaryRecord
{
    std::vector<int>   slot_ids;
    bool               complete = true;
    std::vector<Issue> issues;
};

struct VolumeRecord
{
    std::string        id;
    const ModelVolume* volume = nullptr;
    size_t             object_index = 0;
};

struct ObjectRecord
{
    std::string         id;
    const ModelObject*  object = nullptr;
    std::vector<size_t> volume_indices;
    std::vector<size_t> instance_indices;
};

struct InstanceRecord
{
    std::string          id;
    const ModelInstance* instance = nullptr;
    size_t               object_index = 0;
    int                  plate_index = -1; // -1: not assigned to any plate
};

struct PlateRecord
{
    std::string         id;
    int                 index = 0;
    std::string         name;
    std::optional<bool> locked;
    // Plate-to-world frame, recovered from the imported bed configuration.
    // Absent when no reliable frame could be recovered.
    std::optional<TransformValue>      to_world;
    std::map<std::string, std::string> config;
    std::vector<size_t>                instance_indices;
};

class ProjectSnapshot
{
public:
    // Read `path` with Orca's native importers and retain the resulting project.
    // Throws FileNotFoundError/PermissionError (as OSError subclasses),
    // ProjectReadError or UnsupportedProjectError through the pybind translators.
    static std::shared_ptr<ProjectSnapshot> read_file(const std::string& path);

    ~ProjectSnapshot();

    ProjectSnapshot(const ProjectSnapshot&)            = delete;
    ProjectSnapshot& operator=(const ProjectSnapshot&) = delete;

    // --- Retained native state ------------------------------------------
    // Later native operations (analysis, slicing) consume this directly or take
    // a native copy of it. They must not rebuild a project from the normalized
    // records below, and must not reopen source_path.
    const Model&              model() const { return m_model; }
    const DynamicPrintConfig& config() const { return m_config; }
    const PlateDataPtrs&      plate_data() const { return m_plates; }
    const std::vector<Preset*>& embedded_presets() const { return m_presets; }

    // --- Normalized graph ------------------------------------------------
    const std::string& source_path() const { return m_source_path; }
    const std::string& source_kind() const { return m_source_kind; }
    const std::map<std::string, std::string>& metadata() const { return m_metadata; }
    const std::vector<ObjectRecord>&    objects() const { return m_objects; }
    const std::vector<VolumeRecord>&    volumes() const { return m_volumes; }
    const std::vector<InstanceRecord>&  instances() const { return m_instances; }
    const std::vector<PlateRecord>&     plates() const { return m_plate_records; }
    const std::vector<size_t>&          unassigned_instances() const { return m_unassigned; }
    const std::vector<MaterialRecord>&  materials() const { return m_materials; }
    const std::vector<Issue>&           issues() const { return m_issues; }

    // Serialized project-scope settings, one entry per set key.
    std::map<std::string, std::string> serialized_config() const;

    // Slot lookup by one-based slot id; nullptr when the slot is unknown.
    const MaterialRecord* material(int slot_id) const;

    // Assignment of `volume_index` within `instance_index`.
    MaterialAssignmentRecord assignment(size_t instance_index, size_t volume_index) const;
    // Deduplicated inventory over a set of instances.
    MaterialSummaryRecord    summary(const std::vector<size_t>& instance_indices) const;

    // World-space transform/bounds of one instance.
    TransformValue instance_transform_world(size_t instance_index) const;
    BoundingBoxf3  instance_bounds_world(size_t instance_index) const;
    // Plate-space equivalents. Throw CoordinateUnavailableError when the instance
    // has no plate or its plate has no recoverable frame.
    TransformValue instance_transform_plate(size_t instance_index) const;
    BoundingBoxf3  instance_bounds_plate(size_t instance_index) const;

private:
    ProjectSnapshot() = default;

    void build_graph();
    void build_plates();
    void build_materials();

    // Owned native state.
    Model                m_model;
    DynamicPrintConfig   m_config;
    PlateDataPtrs        m_plates;
    std::vector<Preset*> m_presets;

    std::string m_source_path;
    std::string m_source_kind = "file";
    unsigned    m_serial      = 0;

    std::map<std::string, std::string> m_metadata;
    std::vector<ObjectRecord>          m_objects;
    std::vector<VolumeRecord>          m_volumes;
    std::vector<InstanceRecord>        m_instances;
    std::vector<PlateRecord>           m_plate_records;
    std::vector<size_t>                m_unassigned;
    std::vector<MaterialRecord>        m_materials;
    std::vector<Issue>                 m_issues;
};

using ProjectSnapshotPtr = std::shared_ptr<ProjectSnapshot>;

// The C++ type behind `orca.host.project.Project`. It carries no behaviour of its
// own: it exists so the snapshot can be reached from native code that already has
// the Python object, without publishing a pointer accessor to scripts.
struct ProjectHandle
{
    ProjectSnapshotPtr snapshot;
};

} // namespace host_project
} // namespace Slic3r
