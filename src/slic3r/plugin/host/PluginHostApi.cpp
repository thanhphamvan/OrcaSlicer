#include "PluginHostApi.hpp"

#include <cerrno>
#include <mutex>
#include <unordered_map>

namespace py = pybind11;

namespace Slic3r {
namespace host_api {
namespace {

std::mutex&           registry_mutex() { static std::mutex m; return m; }
std::set<std::string>& registry()      { static std::set<std::string> s; return s; }

// Python exception classes, keyed by their attribute name on orca.host.errors.
// pybind keeps the class objects alive for the process, so a borrowed handle is
// enough here and avoids a decref after interpreter finalization.
std::unordered_map<std::string, py::handle>& error_classes()
{
    static std::unordered_map<std::string, py::handle> classes;
    return classes;
}

// Register one exception class and its translator. `base` is the Python base
// class, so the documented hierarchy (UnsupportedProjectError -> ProjectReadError
// -> RuntimeError) holds for `except` clauses written against either level.
template<typename CppException>
py::handle add_error(py::module_& errors, const char* name, py::handle base)
{
    const py::handle cls = py::register_exception<CppException>(errors, name, base);
    error_classes()[name] = cls;
    return cls;
}

} // namespace

void enable_capability(const std::string& name)
{
    std::lock_guard<std::mutex> lock(registry_mutex());
    registry().insert(name);
}

void disable_capability(const std::string& name)
{
    std::lock_guard<std::mutex> lock(registry_mutex());
    registry().erase(name);
}

bool has_capability(const std::string& name)
{
    std::lock_guard<std::mutex> lock(registry_mutex());
    return registry().count(name) != 0;
}

std::set<std::string> capabilities()
{
    std::lock_guard<std::mutex> lock(registry_mutex());
    return registry();
}

py::object error_class(const std::string& name)
{
    auto it = error_classes().find(name);
    return it == error_classes().end() ? py::none() : py::reinterpret_borrow<py::object>(it->second);
}

void register_errors(py::module_& host)
{
    auto errors = host.def_submodule("errors",
        "Exceptions shared by the orca.host automation API.\n"
        "Every class here derives from RuntimeError, except CoordinateUnavailableError\n"
        "which derives from ValueError. Namespaces that raise one of these re-export it\n"
        "under the same name, as an alias to this identical class object.");

    // Base classes first: a translator registered later runs earlier, so the
    // derived types below take precedence over their bases.
    const py::handle capability = add_error<CapabilityUnavailableError>(errors, "CapabilityUnavailableError",
                                                                       PyExc_RuntimeError);
    add_error<ApplicationUnavailableError>(errors, "ApplicationUnavailableError", capability);
    const py::handle project_read = add_error<ProjectReadError>(errors, "ProjectReadError", PyExc_RuntimeError);
    add_error<UnsupportedProjectError>(errors, "UnsupportedProjectError", project_read);
    add_error<CoordinateUnavailableError>(errors, "CoordinateUnavailableError", PyExc_ValueError);

    // HostOsError has no class of its own: it reproduces the standard OSError
    // subclass for its errno, so scripts catch FileNotFoundError as they would
    // from open(). Registered last, so it wins over the generic translators.
    py::register_exception_translator([](std::exception_ptr p) {
        try {
            if (p)
                std::rethrow_exception(p);
        } catch (const HostOsError& e) {
            errno = e.errno_value;
            PyErr_SetFromErrnoWithFilename(PyExc_OSError, e.path.c_str());
        }
    });
}

void register_discovery(py::module_& host)
{
    host.attr("api_version") = py::make_tuple(api_version_major, api_version_minor);

    host.def(
        "capabilities",
        []() {
            py::set names;
            for (const std::string& name : capabilities())
                names.add(py::str(name));
            return py::frozenset(names);
        },
        "Service capabilities implemented AND usable in this process, as a frozenset.\n"
        "Availability can change in a running GUI, so every call still validates its own\n"
        "context: a successful capability check is not a guarantee for a later call.");
}

} // namespace host_api
} // namespace Slic3r
