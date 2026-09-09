#include "PluginHostProject.hpp"

#include "PluginHostApi.hpp"
#include "slic3r/GUI/PlateGrid.hpp"

#include <libslic3r/BuildVolume.hpp>
#include <libslic3r/Geometry.hpp>
#include <libslic3r/Semver.hpp>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/cstdio.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <set>

namespace Slic3r {
namespace host_project {
namespace {

// Every snapshot gets its own serial so entity IDs from two snapshots never
// collide, which is what makes an accidental cross-snapshot join fail loudly.
unsigned next_serial()
{
    static std::atomic<unsigned> counter{0};
    return ++counter;
}

std::string entity_id(const char* kind, unsigned serial, unsigned long long value)
{
    return std::string(kind) + "-" + std::to_string(serial) + "-" + std::to_string(value);
}

// Validate before any interpretation: a missing or unreadable input is an
// ordinary I/O error, not a malformed project.
void check_readable_regular_file(const std::string& path)
{
    boost::system::error_code ec;
    const boost::filesystem::file_status status = boost::filesystem::status(boost::filesystem::path(path), ec);
    if (ec || status.type() == boost::filesystem::file_not_found)
        throw host_api::HostOsError(ENOENT, path);
    if (status.type() == boost::filesystem::directory_file)
        throw host_api::HostOsError(EISDIR, path);
    FILE* probe = boost::nowide::fopen(path.c_str(), "rb");
    if (probe == nullptr)
        throw host_api::HostOsError(errno, path);
    std::fclose(probe);
}

bool is_archive_path(const std::string& path)
{
    return boost::algorithm::iends_with(path, ".3mf") || boost::algorithm::iends_with(path, ".zip.amf");
}

std::optional<std::string> normalized_color(const std::string& raw)
{
    if (raw.size() != 7 && raw.size() != 9)
        return std::nullopt;
    if (raw.front() != '#')
        return std::nullopt;
    std::string out = "#";
    for (size_t i = 1; i < raw.size(); ++i) {
        if (std::isxdigit(static_cast<unsigned char>(raw[i])) == 0)
            return std::nullopt;
        out += static_cast<char>(std::toupper(static_cast<unsigned char>(raw[i])));
    }
    return out;
}

// A finite, strictly positive measurement, or nothing. Invalid data must stay
// unknown rather than be reported as a plausible default.
std::optional<double> positive_measure(double value)
{
    if (!std::isfinite(value) || value <= 0.0)
        return std::nullopt;
    return value;
}

std::vector<std::string> config_strings(const ConfigBase& config, const char* key)
{
    const ConfigOption* option = config.option(key);
    if (option == nullptr)
        return {};
    if (const auto* strings = dynamic_cast<const ConfigOptionStrings*>(option))
        return strings->values;
    return {};
}

std::vector<double> config_floats(const ConfigBase& config, const char* key)
{
    const ConfigOption* option = config.option(key);
    if (option == nullptr)
        return {};
    if (const auto* floats = dynamic_cast<const ConfigOptionFloats*>(option))
        return floats->values;
    return {};
}

std::vector<unsigned char> config_bools(const DynamicPrintConfig& config, const char* key)
{
    const ConfigOption* option = config.option(key);
    if (option == nullptr)
        return {};
    if (const auto* bools = dynamic_cast<const ConfigOptionBools*>(option))
        return bools->values;
    return {};
}

// "1,3" -> {1, 3}. Unparsable entries are skipped rather than guessed at.
std::vector<int> parse_slot_list(const std::string& raw)
{
    std::vector<int> slots;
    size_t           start = 0;
    while (start <= raw.size()) {
        const size_t comma = raw.find(',', start);
        const std::string token = raw.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        try {
            if (!token.empty())
                slots.push_back(std::stoi(token));
        } catch (const std::exception&) {
            // Not a slot reference; leave it out of the component list.
        }
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    return slots;
}

std::map<std::string, std::string> serialize(const ConfigBase& config)
{
    std::map<std::string, std::string> out;
    for (const std::string& key : config.keys())
        out.emplace(key, config.opt_serialize(key));
    return out;
}

// Bounds over the object's model parts, with `base` applied in front of every
// volume transform. Mirrors ModelObject::instance_bounding_box, which does the
// same for the instance frame, so world bounds stay identical to the native ones.
BoundingBoxf3 model_part_bounds(const ModelObject& object, const Transform3d& base)
{
    BoundingBoxf3 bounds;
    for (const ModelVolume* volume : object.volumes)
        if (volume->is_model_part())
            bounds.merge(volume->mesh().transformed_bounding_box(base * volume->get_matrix()));
    return bounds;
}

} // namespace

TransformValue TransformValue::from_matrix(const Transform3d& matrix)
{
    TransformValue value;
    value.matrix   = matrix;
    value.position = matrix.translation();

    const Geometry::Transformation transformation(matrix);
    // A sheared or singular transform has no faithful TRS form; rotation, scale
    // and mirror are then unavailable together and only the matrix is meaningful.
    const double determinant = matrix.linear().determinant();
    if (transformation.has_skew() || !std::isfinite(determinant) || std::abs(determinant) < EPSILON)
        return value;

    value.decomposed   = true;
    value.rotation_deg = transformation.get_rotation() * (180.0 / PI);
    value.scale        = transformation.get_scaling_factor();
    value.mirror       = transformation.get_mirror();
    return value;
}

ProjectSnapshot::~ProjectSnapshot()
{
    release_PlateData_list(m_plates);
    for (Preset* preset : m_presets)
        delete preset;
}

std::shared_ptr<ProjectSnapshot> ProjectSnapshot::read_file(const std::string& path)
{
    check_readable_regular_file(path);

    boost::system::error_code ec;
    boost::filesystem::path   absolute = boost::filesystem::absolute(boost::filesystem::path(path));
    boost::filesystem::path   resolved = boost::filesystem::canonical(absolute, ec);
    if (!ec)
        absolute = resolved;

    std::shared_ptr<ProjectSnapshot> snapshot(new ProjectSnapshot());
    snapshot->m_serial      = next_serial();
    snapshot->m_source_path = absolute.string();

    ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::EnableSilent);
    const std::string         native_path = absolute.string();

    try {
        if (is_archive_path(native_path)) {
            // Format detection, version handling, plate data and embedded presets all
            // come from the existing importer. Nothing here reads the archive itself.
            En3mfType type = En3mfType::From_Other;
            Semver    version;
            snapshot->m_model = Model::read_from_archive(native_path, &snapshot->m_config, &substitutions, type,
                                                        LoadStrategy::LoadModel | LoadStrategy::LoadConfig,
                                                        &snapshot->m_plates, &snapshot->m_presets, &version);
        } else {
            // Geometry-only formats carry no build items, so the native default
            // instance is what places the single copy this project has.
            snapshot->m_model = Model::read_from_file(native_path, &snapshot->m_config, &substitutions,
                                                      LoadStrategy::LoadModel | LoadStrategy::LoadConfig |
                                                          LoadStrategy::AddDefaultInstances,
                                                      &snapshot->m_plates, &snapshot->m_presets);
        }
    } catch (const host_api::HostOsError&) {
        throw;
    } catch (const std::exception& error) {
        const std::string message(error.what());
        // "Unknown file format" is the importer's way of saying it has no loader
        // for this input; everything else it raises is a failure to read content.
        if (message.find("Unknown file format") != std::string::npos)
            throw host_api::UnsupportedProjectError(message + " (" + native_path + ")");
        throw host_api::ProjectReadError(message + " (" + native_path + ")");
    }

    for (const ConfigSubstitution& substitution : substitutions.substitutions)
        snapshot->m_issues.push_back({"config_substituted",
                                      "Setting '" + (substitution.opt_def ? std::string(substitution.opt_def->opt_key) : std::string("?")) +
                                          "' was substituted while loading the project",
                                      std::string()});

    snapshot->build_graph();
    snapshot->build_plates();
    snapshot->build_materials();
    return snapshot;
}

void ProjectSnapshot::build_graph()
{
    if (m_model.model_info) {
        const ModelInfo& info = *m_model.model_info;
        auto add = [this](const char* key, const std::string& value) {
            if (!value.empty())
                m_metadata.emplace(key, value);
        };
        add("model_name", info.model_name);
        add("description", info.description);
        add("license", info.license);
        add("copyright", info.copyright);
        add("origin", info.origin);
        add("cover_file", info.cover_file);
        for (const auto& item : info.metadata_items)
            if (!item.second.empty())
                m_metadata.emplace(item.first, item.second);
    }
    if (m_model.design_info) {
        if (!m_model.design_info->Designer.empty())
            m_metadata.emplace("designer", m_model.design_info->Designer);
        if (!m_model.design_info->DesignerUserId.empty())
            m_metadata.emplace("designer_user_id", m_model.design_info->DesignerUserId);
    }
    if (!m_model.stl_design_id.empty())
        m_metadata.emplace("design_id", m_model.stl_design_id);

    // Objects, volumes and instances keep native import order.
    m_objects.reserve(m_model.objects.size());
    for (size_t o = 0; o < m_model.objects.size(); ++o) {
        const ModelObject* object = m_model.objects[o];
        ObjectRecord       record;
        record.id     = entity_id("object", m_serial, object->id().id);
        record.object = object;
        for (const ModelVolume* volume : object->volumes) {
            record.volume_indices.push_back(m_volumes.size());
            m_volumes.push_back({entity_id("volume", m_serial, volume->id().id), volume, o});
        }
        for (const ModelInstance* instance : object->instances) {
            record.instance_indices.push_back(m_instances.size());
            m_instances.push_back({entity_id("instance", m_serial, instance->id().id), instance, o, -1});
        }
        // Warm the object's lazily built raw-mesh bounding box while the model is
        // still exclusively ours, so later concurrent reads never race on the cache.
        (void) object->raw_mesh_bounding_box();
        m_objects.push_back(std::move(record));
    }
}

void ProjectSnapshot::build_plates()
{
    if (m_plates.empty()) {
        // A file without plate metadata has no beds to speak of. Every copy is
        // unassigned; inventing a bed or grouping by proximity would be a guess.
        for (size_t i = 0; i < m_instances.size(); ++i)
            m_unassigned.push_back(i);
        if (!m_instances.empty())
            m_issues.push_back({"plate_metadata_missing",
                                "The project carries no plate metadata, so every instance is unassigned",
                                std::string()});
        return;
    }

    // Plate frames follow the same grid rule the application lays plates out on,
    // sized from the project's own bed configuration. Without a usable printable
    // area the frame stays unknown rather than guessed.
    std::optional<Vec2d> cell_size;
    if (const auto* area = m_config.option<ConfigOptionPoints>("printable_area")) {
        const double height = m_config.option("printable_height") != nullptr ? m_config.opt_float("printable_height") : 0.0;
        const BuildVolume volume(area->values, height, {}, {});
        const Vec3d       size = volume.bounding_volume().size();
        if (size.x() > 0.0 && size.y() > 0.0)
            cell_size = Vec2d(size.x(), size.y());
    }
    if (!cell_size)
        m_issues.push_back({"plate_frame_unavailable",
                            "The project has no usable printable area, so plate coordinates are unavailable",
                            std::string()});

    const int columns = compute_colum_count(static_cast<int>(m_plates.size()));

    // Membership comes from the archive's own plate entries, resolved to model
    // indices by the importer. A copy referenced by two plates stays on the first
    // and is reported, so nothing is silently counted twice or dropped.
    std::vector<int> owner(m_instances.size(), -1);
    m_plate_records.reserve(m_plates.size());
    for (size_t p = 0; p < m_plates.size(); ++p) {
        const PlateData* data = m_plates[p];
        PlateRecord      record;
        record.index  = static_cast<int>(p);
        record.id     = entity_id("plate", m_serial, p);
        record.name   = data != nullptr ? data->plate_name : std::string();
        record.locked = data != nullptr ? std::optional<bool>(data->locked) : std::nullopt;
        if (data != nullptr)
            record.config = serialize(data->config);
        if (cell_size)
            record.to_world = TransformValue::from_matrix(Geometry::translation_transform(
                compute_plate_origin(static_cast<int>(p), columns, cell_size->x(), cell_size->y())));

        if (data != nullptr) {
            for (const std::pair<int, int>& member : data->objects_and_instances) {
                if (member.first < 0 || member.first >= static_cast<int>(m_objects.size())) {
                    m_issues.push_back({"plate_membership_broken",
                                        "A plate references an object that is not present in the project", record.id});
                    continue;
                }
                const ObjectRecord& object = m_objects[member.first];
                if (member.second < 0 || member.second >= static_cast<int>(object.instance_indices.size())) {
                    m_issues.push_back({"plate_membership_broken",
                                        "A plate references a copy that is not present in the project", record.id});
                    continue;
                }
                const size_t instance_index = object.instance_indices[member.second];
                if (owner[instance_index] >= 0) {
                    m_issues.push_back({"plate_membership_conflict",
                                        "A copy is claimed by more than one plate; it stays on the first one",
                                        m_instances[instance_index].id});
                    continue;
                }
                owner[instance_index] = static_cast<int>(p);
            }
        }
        m_plate_records.push_back(std::move(record));
    }

    // A plate's instances are the project instance sequence filtered by membership,
    // so ordering is import order rather than the order the archive listed them in.
    for (size_t i = 0; i < m_instances.size(); ++i) {
        if (owner[i] < 0) {
            m_unassigned.push_back(i);
            continue;
        }
        m_instances[i].plate_index = owner[i];
        m_plate_records[owner[i]].instance_indices.push_back(i);
    }
}

void ProjectSnapshot::build_materials()
{
    const std::vector<std::string> presets   = config_strings(m_config, "filament_settings_id");
    const std::vector<std::string> types     = config_strings(m_config, "filament_type");
    const std::vector<std::string> vendors   = config_strings(m_config, "filament_vendor");
    const std::vector<std::string> colors    = config_strings(m_config, "filament_colour");
    const std::vector<double>      densities = config_floats(m_config, "filament_density");
    const std::vector<double>      diameters = config_floats(m_config, "filament_diameter");
    const std::vector<unsigned char> mixed   = config_bools(m_config, "filament_is_mixed");
    const std::vector<std::string> components = config_strings(m_config, "filament_mixed_components");

    size_t configured = 0;
    for (size_t size : {presets.size(), types.size(), vendors.size(), colors.size(), densities.size(), diameters.size()})
        configured = std::max(configured, size);

    // Presets embedded in the archive fill the fields the project config does not
    // carry. Installed or user-selected presets are never consulted for a file read.
    std::map<std::string, const DynamicPrintConfig*> embedded;
    for (const Preset* preset : m_presets)
        if (preset != nullptr && preset->type == Preset::TYPE_FILAMENT)
            embedded.emplace(preset->name, &preset->config);

    // Vector settings are indexed by slot, so a material's own config view is the
    // project's filament settings narrowed to that slot.
    auto slot_config = [this](int slot_id) {
        std::map<std::string, std::string> out;
        for (const std::string& key : m_config.keys()) {
            if (!boost::algorithm::starts_with(key, "filament_"))
                continue;
            const ConfigOption* option = m_config.option(key);
            if (option == nullptr)
                continue;
            if (option->is_vector()) {
                const auto* vector = static_cast<const ConfigOptionVectorBase*>(option);
                if (static_cast<size_t>(slot_id) > vector->size())
                    continue;
                out.emplace(key, vector->vserialize()[slot_id - 1]);
            } else {
                out.emplace(key, option->serialize());
            }
        }
        return out;
    };

    m_materials.reserve(configured);
    for (size_t index = 0; index < configured; ++index) {
        MaterialRecord material;
        material.slot_id = static_cast<int>(index) + 1;
        material.kind    = "physical";

        // Fields resolve from the project config first, then the embedded preset for
        // this slot, and stay unavailable rather than being defaulted.
        const DynamicPrintConfig* preset_config = nullptr;
        if (index < presets.size()) {
            auto found = embedded.find(presets[index]);
            if (found != embedded.end())
                preset_config = found->second;
        }
        auto take_string = [&](const std::vector<std::string>& values, const char* field, const char* key,
                               std::optional<std::string>& target) {
            if (index < values.size() && !values[index].empty()) {
                target                        = values[index];
                material.field_sources[field] = "project_config";
                return;
            }
            if (preset_config != nullptr) {
                const std::vector<std::string> from_preset = config_strings(*preset_config, key);
                if (!from_preset.empty() && !from_preset.front().empty()) {
                    target                        = from_preset.front();
                    material.field_sources[field] = "embedded_preset";
                    return;
                }
            }
            material.field_sources[field] = "unavailable";
        };
        auto take_measure = [&](const std::vector<double>& values, const char* field, const char* key,
                                std::optional<double>& target) {
            if (index < values.size())
                target = positive_measure(values[index]);
            if (target) {
                material.field_sources[field] = "project_config";
                return;
            }
            if (preset_config != nullptr) {
                const std::vector<double> from_preset = config_floats(*preset_config, key);
                if (!from_preset.empty())
                    target = positive_measure(from_preset.front());
                if (target) {
                    material.field_sources[field] = "embedded_preset";
                    return;
                }
            }
            material.field_sources[field] = "unavailable";
        };
        take_string(presets, "preset_name", "filament_settings_id", material.preset_name);
        take_string(types, "material_type", "filament_type", material.material_type);
        take_string(vendors, "vendor", "filament_vendor", material.vendor);

        // The archive has no display name separate from the profile label, so the
        // label is what a user sees for this slot. Reported with its real source.
        material.name                    = material.preset_name;
        material.field_sources["name"]   = material.field_sources["preset_name"];

        std::optional<std::string> raw_color;
        take_string(colors, "color", "filament_colour", raw_color);
        if (raw_color) {
            material.color = normalized_color(*raw_color);
            if (!material.color) {
                material.field_sources["color"] = "unavailable";
                m_issues.push_back({"material_color_unrecognized",
                                    "Filament colour '" + *raw_color + "' is not a recognized #RRGGBB value",
                                    std::string()});
            }
        }

        take_measure(densities, "density_g_cm3", "filament_density", material.density_g_cm3);
        take_measure(diameters, "diameter_mm", "filament_diameter", material.diameter_mm);

        // A mixed slot is virtual: it keeps its own ID and names the physical
        // components it blends, which callers follow explicitly. Its density is
        // never computed from an unstated mixing ratio.
        if (index < mixed.size() && mixed[index] != 0) {
            material.kind = "mixed";
            if (index < components.size())
                material.component_slot_ids = parse_slot_list(components[index]);
        }

        material.config = slot_config(material.slot_id);
        m_materials.push_back(std::move(material));
    }

    // A part may reference a slot the project never configured. Keep the reference
    // visible as an explicitly unknown slot instead of silently retargeting it.
    std::set<int> referenced;
    for (size_t i = 0; i < m_instances.size(); ++i) {
        const ObjectRecord& object = m_objects[m_instances[i].object_index];
        for (size_t v : object.volume_indices) {
            const MaterialAssignmentRecord record = assignment(i, v);
            if (record.default_slot_id)
                referenced.insert(*record.default_slot_id);
            referenced.insert(record.painted_slot_ids.begin(), record.painted_slot_ids.end());
        }
    }
    for (int slot_id : referenced) {
        if (slot_id <= static_cast<int>(configured))
            continue;
        MaterialRecord unknown;
        unknown.slot_id = slot_id;
        unknown.kind    = "unknown";
        for (const char* field : {"name", "preset_name", "material_type", "vendor", "color", "density_g_cm3", "diameter_mm"})
            unknown.field_sources[field] = "unavailable";
        m_materials.push_back(std::move(unknown));
        m_issues.push_back({"material_slot_unknown",
                            "Slot " + std::to_string(slot_id) + " is referenced by geometry but not configured",
                            std::string()});
    }
    std::sort(m_materials.begin(), m_materials.end(),
              [](const MaterialRecord& a, const MaterialRecord& b) { return a.slot_id < b.slot_id; });
}

std::map<std::string, std::string> ProjectSnapshot::serialized_config() const { return serialize(m_config); }

const MaterialRecord* ProjectSnapshot::material(int slot_id) const
{
    for (const MaterialRecord& record : m_materials)
        if (record.slot_id == slot_id)
            return &record;
    return nullptr;
}

MaterialAssignmentRecord ProjectSnapshot::assignment(size_t instance_index, size_t volume_index) const
{
    const ModelVolume* volume = m_volumes[volume_index].volume;
    MaterialAssignmentRecord record;

    if (!volume->is_model_part()) {
        // Modifiers, negative volumes and support blockers are not extruded as
        // parts, so their stored slot override carries no assignment.
        record.source   = "not_applicable";
        record.complete = true;
        return record;
    }

    // Native inheritance: a volume override wins, then the object's, then the
    // project default. extruder == 0 means "inherit" and is never a valid slot.
    const ConfigOption* own = volume->config.option("extruder");
    if (own != nullptr && own->getInt() > 0) {
        record.default_slot_id = own->getInt();
        record.source          = "volume";
    } else if (const ConfigOption* inherited = volume->get_object()->config.option("extruder");
               inherited != nullptr && inherited->getInt() > 0) {
        record.default_slot_id = inherited->getInt();
        record.source          = "object";
    } else if (!m_materials.empty() || m_config.option("filament_settings_id") != nullptr) {
        record.default_slot_id = 1;
        record.source          = "project_default";
    } else {
        record.source   = "unknown";
        record.complete = false;
    }

    if (volume->is_mm_painted()) {
        record.is_painted = true;
        for (size_t slot : volume->get_extruders_from_multi_material_painting())
            record.painted_slot_ids.push_back(static_cast<int>(slot));
        std::sort(record.painted_slot_ids.begin(), record.painted_slot_ids.end());
        record.painted_slot_ids.erase(std::unique(record.painted_slot_ids.begin(), record.painted_slot_ids.end()),
                                      record.painted_slot_ids.end());
        if (record.painted_slot_ids.empty())
            record.complete = false;
    }
    (void) instance_index; // painting and overrides are per volume, not per copy
    return record;
}

MaterialSummaryRecord ProjectSnapshot::summary(const std::vector<size_t>& instance_indices) const
{
    MaterialSummaryRecord summary;
    std::set<int>         slots;
    for (size_t instance_index : instance_indices) {
        const ObjectRecord& object = m_objects[m_instances[instance_index].object_index];
        for (size_t volume_index : object.volume_indices) {
            const MaterialAssignmentRecord record = assignment(instance_index, volume_index);
            if (record.source == "not_applicable")
                continue;
            if (record.default_slot_id)
                slots.insert(*record.default_slot_id);
            slots.insert(record.painted_slot_ids.begin(), record.painted_slot_ids.end());
            if (!record.complete) {
                summary.complete = false;
                summary.issues.push_back({"material_assignment_incomplete",
                                          "The referenced slots of a part could not be fully resolved",
                                          m_volumes[volume_index].id});
            }
        }
    }
    summary.slot_ids.assign(slots.begin(), slots.end());
    return summary;
}

TransformValue ProjectSnapshot::instance_transform_world(size_t instance_index) const
{
    return TransformValue::from_matrix(m_instances[instance_index].instance->get_matrix());
}

BoundingBoxf3 ProjectSnapshot::instance_bounds_world(size_t instance_index) const
{
    const InstanceRecord& record = m_instances[instance_index];
    return m_objects[record.object_index].object->instance_bounding_box(*record.instance);
}

namespace {

// The plate frame an instance is expressed in, or a coordinate error explaining
// which half is missing: membership, or a recoverable frame for its plate.
const TransformValue& require_plate_frame(const ProjectSnapshot& snapshot, const InstanceRecord& record)
{
    if (record.plate_index < 0)
        throw host_api::CoordinateUnavailableError("This copy is not assigned to a plate, so it has no plate coordinates");
    const PlateRecord& plate = snapshot.plates()[record.plate_index];
    if (!plate.to_world)
        throw host_api::CoordinateUnavailableError("No reliable frame could be recovered for plate " +
                                                   std::to_string(plate.index));
    return *plate.to_world;
}

} // namespace

TransformValue ProjectSnapshot::instance_transform_plate(size_t instance_index) const
{
    const InstanceRecord& record = m_instances[instance_index];
    const TransformValue& frame  = require_plate_frame(*this, record);
    return TransformValue::from_matrix(frame.matrix.inverse() * record.instance->get_matrix());
}

BoundingBoxf3 ProjectSnapshot::instance_bounds_plate(size_t instance_index) const
{
    const InstanceRecord& record = m_instances[instance_index];
    const TransformValue& frame  = require_plate_frame(*this, record);
    return model_part_bounds(*m_objects[record.object_index].object, frame.matrix.inverse() * record.instance->get_matrix());
}

} // namespace host_project
} // namespace Slic3r
