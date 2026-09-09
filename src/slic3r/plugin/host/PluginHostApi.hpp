#pragma once

// Version and capability discovery for the `orca.host` automation API, plus the
// shared exception hierarchy exposed as `orca.host.errors`.
//
// The version describes this host contract, independently of the application
// version: additive changes bump the minor, incompatible ones the major.
// Capabilities describe what is actually usable in *this* process. A service
// that is not implemented, or not reachable in the current context, must not be
// advertised — see docs/backlog/python-api-contract.md.

#include <pybind11/pybind11.h>

#include <set>
#include <stdexcept>
#include <string>

namespace Slic3r {
namespace host_api {

// --- Version ------------------------------------------------------------

constexpr int api_version_major = 1;
constexpr int api_version_minor = 0;

// --- Capability registry ------------------------------------------------

// Capability names defined by the contract. Only the ones enabled at runtime are
// returned by orca.host.capabilities().
constexpr const char* CAP_SCRIPT_EXECUTE = "script.execute";
constexpr const char* CAP_PROJECT_READ   = "project.read";

// Enable a capability for this process. Called by whoever brings the underlying
// service up (the CLI script runner enables script.execute; the project
// bindings enable project.read when they register).
void enable_capability(const std::string& name);
void disable_capability(const std::string& name);
bool has_capability(const std::string& name);
std::set<std::string> capabilities();

// --- Shared exceptions --------------------------------------------------

// All of these derive from RuntimeError on the Python side unless noted.
struct HostError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// A service exists in the API but is not usable in this process/session.
struct CapabilityUnavailableError : HostError
{
    using HostError::HostError;
};

// The specific case of a GUI-dependent call made without a running application.
struct ApplicationUnavailableError : CapabilityUnavailableError
{
    using CapabilityUnavailableError::CapabilityUnavailableError;
};

// The native loader refused the project (corrupt archive, unreadable content).
struct ProjectReadError : HostError
{
    using HostError::HostError;
};

// The project is well formed but requires something this version cannot honour.
struct UnsupportedProjectError : ProjectReadError
{
    using ProjectReadError::ProjectReadError;
};

// A coordinate space cannot be produced for this entity (no plate membership,
// or no recoverable plate frame). Derives from ValueError on the Python side.
struct CoordinateUnavailableError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// An ordinary filesystem failure, carried as errno + path so services can raise
// it without holding the GIL. Translated into the matching OSError subclass
// (FileNotFoundError, PermissionError, IsADirectoryError, ...).
struct HostOsError : std::runtime_error
{
    HostOsError(int error_number, std::string file)
        : std::runtime_error("I/O error on " + file), errno_value(error_number), path(std::move(file))
    {}

    int         errno_value;
    std::string path;
};

// Create the orca.host.errors submodule and register the exception translators.
// Must run before any registrar that raises these types.
void register_errors(pybind11::module_& host);

// Register api_version / capabilities() on orca.host.
void register_discovery(pybind11::module_& host);

// Handles of the registered Python exception classes, for re-export as aliases
// (orca.host.project re-exports the names it raises) and for the script runner's
// exit-code mapping. Valid only after register_errors() has run.
pybind11::object error_class(const std::string& name);

} // namespace host_api
} // namespace Slic3r
