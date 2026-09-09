#include <catch2/catch_all.hpp>

#include "python_test_support.hpp"
#include "test_utils.hpp"

#include <libslic3r/Format/3mf.hpp>
#include <libslic3r/Format/bbs_3mf.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/Preset.hpp>
#include <libslic3r/Semver.hpp>
#include <slic3r/GUI/PartPlate.hpp>
#include <slic3r/plugin/host/PluginHostProject.hpp>

#include <pybind11/embed.h>
#include <pybind11/pybind11.h>

#include <set>
#include <string>
#include <utility>
#include <vector>

namespace py = pybind11;

namespace {

py::object project_api()
{
    py::object host = import_orca_module().attr("host");
    REQUIRE(py::hasattr(host, "project"));
    return host.attr("project");
}

std::string unicode_3mf_path()
{
    return std::string(TEST_DATA_DIR) + "/test_3mf/Geräte/Büchse.3mf";
}

// Independent native reference. Never obtain expected geometry from the Python
// implementation, cached metadata, or a second XML parser written for this test.
struct NativeProjectReference {
    Slic3r::Model model;
    Slic3r::DynamicPrintConfig config;
    Slic3r::PlateDataPtrs plates;
    std::vector<Slic3r::Preset*> presets;

    void read(const std::string& path)
    {
        Slic3r::ConfigSubstitutionContext substitutions(Slic3r::ForwardCompatibilitySubstitutionRule::EnableSilent);
        Slic3r::En3mfType type = Slic3r::En3mfType::From_Other;
        Slic3r::Semver version;
        model = Slic3r::Model::read_from_archive(path, &config, &substitutions, type,
            Slic3r::LoadStrategy::LoadModel | Slic3r::LoadStrategy::LoadConfig,
            &plates, &presets, &version);
    }

    ~NativeProjectReference()
    {
        Slic3r::release_PlateData_list(plates);
        for (auto* preset : presets)
            delete preset;
    }
};

void check_vector(py::handle actual, const Slic3r::Vec3d& expected)
{
    py::tuple values = py::reinterpret_borrow<py::object>(actual).cast<py::tuple>();
    REQUIRE(py::len(values) == 3);
    for (int axis = 0; axis < 3; ++axis)
        CHECK_THAT(values[axis].cast<double>(), Catch::Matchers::WithinAbs(expected[axis], 1e-5));
}

void check_matrix(py::handle actual, const Slic3r::Transform3d& expected)
{
    py::tuple rows = py::reinterpret_borrow<py::object>(actual).cast<py::tuple>();
    REQUIRE(py::len(rows) == 4);
    for (int row = 0; row < 4; ++row) {
        py::tuple values = rows[row].cast<py::tuple>();
        REQUIRE(py::len(values) == 4);
        for (int col = 0; col < 4; ++col)
            CHECK_THAT(values[col].cast<double>(), Catch::Matchers::WithinAbs(expected.matrix()(row, col), 1e-6));
    }
}

} // namespace

// The first vertical slice of the project inspection API: capability discovery,
// the immutable graph, native geometry parity, ownership across a released
// project, and plate membership and frames. Expected values come from an
// independent native read or from what the test itself wrote, never from the API
// under test.

TEST_CASE("Python host advertises project inspection only when it is available", "[ProjectInspection][Python]")
{
    py::object host = import_orca_module().attr("host");

    REQUIRE(py::hasattr(host, "api_version"));
    REQUIRE(py::hasattr(host, "capabilities"));

    py::tuple version = host.attr("api_version").cast<py::tuple>();
    REQUIRE(py::len(version) == 2);
    CHECK(version[0].cast<int>() == 1);
    CHECK(version[1].cast<int>() == 0);

    py::object capabilities = host.attr("capabilities")();
    CHECK(py::isinstance<py::frozenset>(capabilities));
    CHECK(capabilities.attr("__contains__")("project.read").cast<bool>());
    // This harness embeds Python but does not enter the CLI runner. Verify
    // script.execute inside a real --script invocation, not by registering a
    // capability unconditionally just to satisfy this test.
}

TEST_CASE("Python project namespace exposes the immutable inspection graph", "[ProjectInspection][Python]")
{
    py::object api = project_api();

    for (const char* name : { "read", "Project", "Plate", "Object", "Volume", "Instance",
                              "Transform", "Material", "MaterialAssignment", "MaterialSummary", "Issue" }) {
        CAPTURE(name);
        CHECK(py::hasattr(api, name));
    }
}

TEST_CASE("Python project reader loads a Unicode 3MF without a GUI", "[ProjectInspection][Python]")
{
    py::object api = project_api();
    REQUIRE(py::hasattr(api, "read"));

    py::object project = api.attr("read")(unicode_3mf_path());
    CHECK(project.attr("source_kind").cast<std::string>() == "file");
    CHECK(project.attr("source_document_id").is_none());
    CHECK(project.attr("source_revision").is_none());

    py::tuple objects = project.attr("objects").cast<py::tuple>();
    py::tuple instances = project.attr("instances").cast<py::tuple>();
    py::tuple plates = project.attr("plates").cast<py::tuple>();
    py::tuple unassigned = project.attr("unassigned_instances").cast<py::tuple>();
    REQUIRE(py::len(objects) > 0);
    REQUIRE(py::len(instances) > 0);

    // This generic 3MF fixture has no Orca/Bambu plate metadata. The reader must
    // preserve that distinction instead of inventing a bed or assigning by proximity.
    CHECK(py::len(plates) == 0);
    CHECK(py::len(unassigned) == py::len(instances));

    py::object instance = instances[0];
    CHECK(instance.attr("plate_id").is_none());
    CHECK(instance.attr("bounds")(py::arg("space") = "world").attr("defined").cast<bool>());

    py::tuple issues = project.attr("issues").cast<py::tuple>();
    bool has_missing_plate_issue = false;
    for (py::handle issue : issues) {
        if (issue.attr("code").cast<std::string>() == "plate_metadata_missing") {
            has_missing_plate_issue = true;
            break;
        }
    }
    CHECK(has_missing_plate_issue);
}

TEST_CASE("Python project children retain their owned snapshot", "[ProjectInspection][Python]")
{
    py::object api = project_api();
    ScopedTemporaryFile source(".3mf");
    boost::filesystem::copy_file(unicode_3mf_path(), source.path());
    py::object project = api.attr("read")(source.string());
    py::object instance = project.attr("instances").cast<py::tuple>()[0];
    const std::string name = instance.attr("object").attr("name").cast<std::string>();

    project = py::none();
    REQUIRE(boost::filesystem::remove(source.path()));
    py::module_::import("gc").attr("collect")();
    py::object other = api.attr("read")(unicode_3mf_path());

    // First mesh access occurs after the source disappears and another read.
    CHECK(instance.attr("object").attr("name").cast<std::string>() == name);
    py::object mesh = instance.attr("object").attr("volumes").cast<py::tuple>()[0].attr("mesh")();
    REQUIRE(py::isinstance<Slic3r::TriangleMesh>(mesh));
    instance = py::none();
    other = py::none();
    py::module_::import("gc").attr("collect")();
    const auto& native_mesh = mesh.cast<const Slic3r::TriangleMesh&>();
    REQUIRE_FALSE(native_mesh.its.vertices.empty());
    REQUIRE_FALSE(native_mesh.its.indices.empty());
    check_vector(mesh.attr("vertex")(0), native_mesh.its.vertices.front().cast<double>());
}

TEST_CASE("Python project geometry matches native imports of generated transformed models", "[ProjectInspection][Python]")
{
    py::object api = project_api();
    const double width = GENERATE(11.0, 23.0);
    Slic3r::Model source;
    auto* object = source.add_object();
    object->name = "Generated native part";
    auto* volume = object->add_volume(Slic3r::make_cube(width, 17.0, 29.0));
    volume->set_offset(Slic3r::Vec3d(3.0, -4.0, 2.0));
    volume->set_rotation(Slic3r::Vec3d(0.1, 0.2, 0.3));
    for (int copy = 0; copy < 2; ++copy) {
        auto* instance = object->add_instance();
        instance->set_offset(Slic3r::Vec3d(width + 40.0 * copy, 60.0, 35.0));
        instance->set_rotation(Slic3r::Vec3d(0.2, 0.3, 0.4 + copy));
        instance->set_scaling_factor(Slic3r::Vec3d(1.2, 0.8, 1.5));
        instance->set_mirror(Slic3r::Vec3d(copy == 0 ? 1.0 : -1.0, 1.0, 1.0));
    }
    ScopedTemporaryFile file(".3mf");
    REQUIRE(Slic3r::store_3mf(file.string().c_str(), &source, nullptr, false));

    NativeProjectReference native;
    native.read(file.string());
    py::object project = api.attr("read")(file.string());
    py::tuple objects = project.attr("objects").cast<py::tuple>();
    REQUIRE(py::len(objects) == native.model.objects.size());
    REQUIRE_FALSE(native.model.objects.empty());
    size_t instance_count = 0;
    for (size_t i = 0; i < native.model.objects.size(); ++i) {
        const auto& expected = *native.model.objects[i];
        py::object actual = objects[i];
        CHECK(actual.attr("name").cast<std::string>() == expected.name);
        py::tuple volumes = actual.attr("volumes").cast<py::tuple>();
        py::tuple instances = actual.attr("instances").cast<py::tuple>();
        REQUIRE(py::len(volumes) == expected.volumes.size());
        REQUIRE(py::len(instances) == expected.instances.size());
        instance_count += expected.instances.size();
        for (size_t v = 0; v < expected.volumes.size(); ++v) {
            py::object mesh = volumes[v].attr("mesh")();
            REQUIRE(py::isinstance<Slic3r::TriangleMesh>(mesh));
            const auto& actual_mesh = mesh.cast<const Slic3r::TriangleMesh&>();
            const auto& expected_mesh = expected.volumes[v]->mesh();
            REQUIRE(actual_mesh.its.vertices.size() == expected_mesh.its.vertices.size());
            REQUIRE(actual_mesh.its.indices.size() == expected_mesh.its.indices.size());
            for (size_t p = 0; p < expected_mesh.its.vertices.size(); ++p)
                CHECK_THAT((actual_mesh.its.vertices[p] - expected_mesh.its.vertices[p]).norm(),
                    Catch::Matchers::WithinAbs(0.0, 1e-5));
            for (size_t t = 0; t < expected_mesh.its.indices.size(); ++t)
                CHECK((actual_mesh.its.indices[t].array() == expected_mesh.its.indices[t].array()).all());
            check_matrix(volumes[v].attr("transform").attr("matrix"), expected.volumes[v]->get_matrix());
        }
        for (size_t j = 0; j < expected.instances.size(); ++j) {
            check_matrix(instances[j].attr("transform")(py::arg("space") = "world").attr("matrix"),
                expected.instances[j]->get_matrix());
            const auto bounds = expected.instance_bounding_box(*expected.instances[j]);
            py::object actual_bounds = instances[j].attr("bounds")(py::arg("space") = "world");
            REQUIRE(actual_bounds.attr("defined").cast<bool>());
            check_vector(actual_bounds.attr("min"), bounds.min);
            check_vector(actual_bounds.attr("max"), bounds.max);
        }
    }
    CHECK(instance_count == 2);
    CHECK(py::len(project.attr("instances")) == instance_count);
}

TEST_CASE("Python project reader maps a missing file to FileNotFoundError", "[ProjectInspection][Python]")
{
    py::object api = project_api();
    const std::string missing = std::string(TEST_DATA_DIR) + "/test_3mf/project-inspection-missing.3mf";

    try {
        api.attr("read")(missing);
        FAIL("Reading a missing project unexpectedly succeeded");
    } catch (const py::error_already_set& error) {
        CHECK(error.matches(PyExc_FileNotFoundError));
    }
}

namespace {

// A project authored natively for one test, written with the application's own
// exporter. Expected values come from what was written here, never from the API
// under test.
struct AuthoredProject
{
    Slic3r::Model              model;
    Slic3r::DynamicPrintConfig config;
    Slic3r::PlateDataPtrs      plates;

    ~AuthoredProject() { Slic3r::release_PlateData_list(plates); }

    Slic3r::ModelObject* add_object(const char* name, double size, size_t copies)
    {
        auto* object = model.add_object();
        object->name = name;
        object->add_volume(Slic3r::make_cube(size, size, size));
        for (size_t copy = 0; copy < copies; ++copy)
            object->add_instance()->set_offset(Slic3r::Vec3d(20.0 * (copy + 1), 30.0, size / 2));
        return object;
    }

    // `members` are (model object index, instance index) pairs, exactly as the
    // exporter writes them into the plate's <model_instance> entries.
    void add_plate(std::vector<std::pair<int, int>> members)
    {
        auto* plate = new Slic3r::PlateData();
        plate->plate_index = static_cast<int>(plates.size());
        plate->objects_and_instances = std::move(members);
        plates.push_back(plate);
    }

    void set_square_bed(double size)
    {
        config.set_key_value("printable_area",
            new Slic3r::ConfigOptionPoints({Slic3r::Vec2d(0, 0), Slic3r::Vec2d(size, 0),
                                            Slic3r::Vec2d(size, size), Slic3r::Vec2d(0, size)}));
        config.set_key_value("printable_height", new Slic3r::ConfigOptionFloat(250.0));
    }

    bool store(const std::string& path)
    {
        // The exporter stages its parts under the temporary directory. Nothing
        // sets one in this harness, so point it at the system temp directory
        // instead of letting it fall back to the filesystem root.
        if (Slic3r::temporary_dir().empty())
            Slic3r::set_temporary_dir(boost::filesystem::temp_directory_path().string());

        Slic3r::StoreParams params;
        params.path            = path;
        params.model           = &model;
        params.plate_data_list = plates;
        params.config          = &config;
        return Slic3r::store_bbs_3mf(params);
    }
};

} // namespace

TEST_CASE("Python project keeps every copy on exactly one plate", "[ProjectInspection][Python]")
{
    py::object      api = project_api();
    AuthoredProject authored;
    authored.set_square_bed(300.0);
    authored.add_object("Twin", 10.0, 2); // object 0: two copies, both on plate 0
    authored.add_object("Single", 8.0, 1); // object 1: one copy, left unassigned
    authored.add_plate({{0, 0}, {0, 1}});
    authored.add_plate({}); // an empty plate is still a plate

    ScopedTemporaryFile file(".3mf");
    REQUIRE(authored.store(file.string()));

    py::object project = api.attr("read")(file.string());
    py::tuple  plates  = project.attr("plates").cast<py::tuple>();
    REQUIRE(py::len(plates) == 2);
    CHECK(py::len(plates[0].attr("instances")) == 2);
    CHECK(py::len(plates[1].attr("instances")) == 0);
    CHECK(plates[1].attr("materials")().attr("complete").cast<bool>());
    CHECK(py::len(plates[1].attr("materials")().attr("items")) == 0);

    // Both copies of one object must survive: obj_inst_map alone would collapse them.
    py::tuple instances = project.attr("instances").cast<py::tuple>();
    CHECK(py::len(instances) == 3);
    py::tuple unassigned = project.attr("unassigned_instances").cast<py::tuple>();
    REQUIRE(py::len(unassigned) == 1);
    CHECK(unassigned[0].attr("object").attr("name").cast<std::string>() == "Single");
    CHECK(unassigned[0].attr("plate_id").is_none());

    std::set<std::string> assigned_ids;
    for (py::handle plate : plates)
        for (py::handle instance : plate.attr("instances"))
            assigned_ids.insert(instance.attr("id").cast<std::string>());
    CHECK(assigned_ids.size() == 2);
    CHECK(assigned_ids.count(unassigned[0].attr("id").cast<std::string>()) == 0);

    // A copy without plate membership has no plate coordinates, and says so.
    CHECK_THROWS_AS(unassigned[0].attr("transform")(py::arg("space") = "plate"), py::error_already_set);
    CHECK(unassigned[0].attr("bounds")(py::arg("space") = "world").attr("defined").cast<bool>());
}

TEST_CASE("Python plate frames match the application's own plate layout", "[ProjectInspection][Python]")
{
    py::object      api = project_api();
    const double    bed = 300.0;
    AuthoredProject authored;
    authored.set_square_bed(bed);
    authored.add_object("Cube", 10.0, 3);
    authored.add_plate({{0, 0}});
    authored.add_plate({{0, 1}});
    authored.add_plate({{0, 2}});

    ScopedTemporaryFile file(".3mf");
    REQUIRE(authored.store(file.string()));

    // Independent reference: the application's own plate list, told only the bed
    // size. It derives the column count and every origin itself.
    Slic3r::Model reference_model;
    Slic3r::GUI::PartPlateList reference(static_cast<int>(bed), static_cast<int>(bed), 250.0, nullptr,
                                         &reference_model, Slic3r::ptFFF);
    while (reference.get_plate_count() < 3)
        reference.create_plate(false);
    REQUIRE(reference.get_plate_count() == 3);

    py::object project = api.attr("read")(file.string());
    py::tuple  plates  = project.attr("plates").cast<py::tuple>();
    REQUIRE(py::len(plates) == 3);
    for (int index = 0; index < 3; ++index) {
        CAPTURE(index);
        py::object frame = plates[index].attr("transform_to_world");
        REQUIRE_FALSE(frame.is_none());
        check_vector(frame.attr("position_mm"), reference.get_plate(index)->get_origin());
    }
}

TEST_CASE("Retained native project state feeds native code after its source is gone",
          "[ProjectInspection][Python]")
{
    py::object      api = project_api();
    AuthoredProject authored;
    authored.set_square_bed(300.0);
    authored.add_object("Consumed", 12.0, 1);
    authored.config.set_key_value("layer_height", new Slic3r::ConfigOptionFloat(0.24));
    authored.add_plate({{0, 0}});

    ScopedTemporaryFile file(".3mf");
    REQUIRE(authored.store(file.string()));
    py::object project = api.attr("read")(file.string());
    REQUIRE(boost::filesystem::remove(file.path()));

    // Internal test access to the owned snapshot. This is deliberately not a
    // public Python API; it exists so native code holding the Python object can
    // consume the retained state without rebuilding it from reported fields.
    const auto& handle = project.cast<const Slic3r::host_project::ProjectHandle&>();
    REQUIRE(handle.snapshot != nullptr);
    const Slic3r::Model& retained = handle.snapshot->model();
    REQUIRE(retained.objects.size() == 1);

    // An existing native geometry operation, run on the retained model with no
    // file to reopen and no reconstruction from the inspection report.
    const Slic3r::TriangleMesh merged = retained.mesh();
    CHECK_FALSE(merged.its.indices.empty());
    const Slic3r::BoundingBoxf3 native_bounds = retained.bounding_box_exact();
    CHECK(native_bounds.defined);
    check_vector(project.attr("instances").cast<py::tuple>()[0]
                     .attr("bounds")(py::arg("space") = "world").attr("min"),
                 native_bounds.min);

    // Imported settings and plate associations are retained alongside the model.
    CHECK(handle.snapshot->config().opt_float("layer_height") == Catch::Approx(0.24));
    REQUIRE(handle.snapshot->plate_data().size() == 1);
    REQUIRE(handle.snapshot->plate_data().front()->objects_and_instances.size() == 1);
    CHECK(handle.snapshot->plate_data().front()->objects_and_instances.front() == std::make_pair(0, 0));
}
