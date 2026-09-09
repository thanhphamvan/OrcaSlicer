#include "PythonScriptRunner.hpp"

#include "PythonInterpreter.hpp"
#include "PythonPluginBridge.hpp"
#include "host/PluginHostApi.hpp"

#include <pybind11/embed.h>
#include <pybind11/pybind11.h>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/iostream.hpp>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace py = pybind11;

namespace Slic3r {
namespace {

// One "--key value" / "--key=value" application option of script mode.
struct OptionMatch
{
    bool        matched = false;
    bool        inline_value = false;
    std::string value;
};

OptionMatch match_option(const std::string& token, const char* name)
{
    const std::string prefix = std::string("--") + name;
    OptionMatch       match;
    if (token == prefix) {
        match.matched = true;
        return match;
    }
    if (token.rfind(prefix + "=", 0) == 0) {
        match.matched      = true;
        match.inline_value = true;
        match.value        = token.substr(prefix.size() + 1);
    }
    return match;
}

// Report an I/O failure the way the shell would, then hand back exit code 4.
int report_io_error(const std::string& what, const std::string& path, int error_number)
{
    boost::nowide::cerr << "OrcaSlicer: " << what << ": " << path << ": " << std::strerror(error_number) << std::endl;
    return SCRIPT_EXIT_IO;
}

// Absolute path for `path`, resolved against the invocation working directory.
// The working directory itself is never changed.
std::string absolute_path(const std::string& path)
{
    boost::system::error_code ec;
    boost::filesystem::path   absolute = boost::filesystem::absolute(boost::filesystem::path(path));
    boost::filesystem::path   resolved = boost::filesystem::canonical(absolute, ec);
    return (ec ? absolute : resolved).string();
}

// A readable regular file, or the errno explaining why not.
bool readable_regular_file(const std::string& path, int& error_number)
{
    boost::system::error_code            ec;
    const boost::filesystem::file_status status = boost::filesystem::status(boost::filesystem::path(path), ec);
    if (ec || status.type() == boost::filesystem::file_not_found) {
        error_number = ENOENT;
        return false;
    }
    if (status.type() == boost::filesystem::directory_file) {
        error_number = EISDIR;
        return false;
    }
    if (status.type() != boost::filesystem::regular_file) {
        error_number = EINVAL;
        return false;
    }
    FILE* probe = boost::nowide::fopen(path.c_str(), "rb");
    if (probe == nullptr) {
        error_number = errno;
        return false;
    }
    std::fclose(probe);
    return true;
}

// Map an uncaught Python exception to an exit code and report it the way the
// interpreter would. Called with the GIL held; consumes `error`.
int report_uncaught(py::error_already_set& error)
{
    if (error.matches(PyExc_SystemExit)) {
        // An explicit exit is intentional, so it takes precedence over the
        // automatic error mapping below.
        const py::object code = error.value().attr("code");
        if (code.is_none())
            return SCRIPT_EXIT_OK;
        if (py::isinstance<py::int_>(code)) {
            const long value = code.cast<long>();
            if (value >= 0 && value <= 125)
                return static_cast<int>(value);
            boost::nowide::cerr << "OrcaSlicer: script exited with status " << value
                                << ", which is outside the supported range 0-125" << std::endl;
            return SCRIPT_EXIT_ERROR;
        }
        try {
            py::print(code, py::arg("file") = py::module_::import("sys").attr("stderr"));
        } catch (const py::error_already_set&) {
            // Reporting the payload must not replace the exit code it explains.
        }
        return SCRIPT_EXIT_ERROR;
    }

    // Classify before printing: PyErr_Print() consumes the exception.
    int code = SCRIPT_EXIT_ERROR;
    if (error.matches(PyExc_KeyboardInterrupt))
        code = SCRIPT_EXIT_INTERRUPTED;
    else if (error.matches(PyExc_OSError))
        code = SCRIPT_EXIT_IO;
    else {
        const py::object project_read_error = host_api::error_class("ProjectReadError");
        if (!project_read_error.is_none() && error.matches(project_read_error))
            code = SCRIPT_EXIT_IO;
    }

    error.restore();
    PyErr_Print();
    return code;
}

} // namespace

ScriptCommandLine parse_script_command_line(int argc, char** argv)
{
    ScriptCommandLine parsed;

    // Application parsing ends at the first "--"; script mode is detected only
    // from the tokens before it.
    int delimiter = argc;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--") {
            delimiter = i;
            break;
        }
    }
    for (int i = delimiter + 1; i < argc; ++i)
        parsed.script_args.emplace_back(argv[i]);

    bool have_script     = false;
    bool have_file       = false;
    bool have_positional = false;
    auto fail            = [&parsed](const std::string& message) {
        if (parsed.error.empty())
            parsed.error = message;
    };

    for (int i = 1; i < delimiter; ++i) {
        const std::string token(argv[i]);

        if (token == "--help") {
            parsed.help_requested = true;
            continue;
        }

        // Reads one option value, from "--key=value" or the following token.
        auto take_value = [&](const OptionMatch& match, const char* name, std::string& target, bool& seen) {
            if (seen) {
                fail(std::string("--") + name + " was given more than once");
                return;
            }
            if (match.inline_value) {
                if (match.value.empty())
                    fail(std::string("--") + name + " requires a path");
                target = match.value;
            } else {
                if (i + 1 >= delimiter) {
                    fail(std::string("--") + name + " requires a path");
                    return;
                }
                target = argv[++i];
            }
            seen = true;
        };

        if (const OptionMatch match = match_option(token, "script"); match.matched) {
            take_value(match, "script", parsed.script_path, have_script);
            parsed.script_mode = true;
            continue;
        }
        if (const OptionMatch match = match_option(token, "file"); match.matched) {
            take_value(match, "file", parsed.input_path, have_file);
            continue;
        }
        if (!token.empty() && token[0] == '-') {
            fail("Invalid option " + token + " in script mode");
            continue;
        }
        if (have_positional) {
            fail("Only one model input is accepted, got another: " + token);
            continue;
        }
        parsed.input_path = token;
        have_positional   = true;
    }

    if (!parsed.script_mode) {
        // Without --script this is not script mode at all: leave the rest of the
        // command line to the existing CLI, unchanged.
        return ScriptCommandLine{};
    }
    if (have_file && have_positional)
        fail("--file and a positional model input cannot be combined");
    if (parsed.script_path.empty() && parsed.error.empty())
        fail("--script requires a path");
    return parsed;
}

int run_python_script(const ScriptCommandLine& command_line)
{
    const std::string script = absolute_path(command_line.script_path);
    int               error_number = 0;
    if (!readable_regular_file(script, error_number))
        return report_io_error("cannot read script", script, error_number);

    std::string input;
    if (!command_line.input_path.empty()) {
        input = absolute_path(command_line.input_path);
        if (!readable_regular_file(input, error_number))
            return report_io_error("cannot read input file", input, error_number);
        // The project itself is only parsed when the script calls read().
    }

    if (!PythonInterpreter::instance().initialize()) {
        boost::nowide::cerr << "OrcaSlicer: the bundled Python runtime is unavailable: "
                            << PythonInterpreter::instance().last_error() << std::endl;
        return SCRIPT_EXIT_RUNTIME_FAILED;
    }

    // Reuse the runtime lease and GIL management the plugin host already uses.
    PythonGILState gil;
    if (!gil) {
        boost::nowide::cerr << "OrcaSlicer: the bundled Python runtime is shutting down" << std::endl;
        return SCRIPT_EXIT_RUNTIME_FAILED;
    }

    int exit_code = SCRIPT_EXIT_OK;
    try {
        // Force the embedded module registration into this process, the same way
        // the plugin host does, then confirm it actually imports.
        (void) PythonPluginBridge::instance();
        py::module_::import("orca");
    } catch (const py::error_already_set& error) {
        boost::nowide::cerr << "OrcaSlicer: the orca Python module failed to initialize: " << error.what() << std::endl;
        return SCRIPT_EXIT_RUNTIME_FAILED;
    }

    // Running a user script is not the same as loading a discovered plugin: it
    // gets its own execution context and stays outside the plugin audit scope.
    host_api::enable_capability(host_api::CAP_SCRIPT_EXECUTE);

    try {
        py::module_ sys = py::module_::import("sys");

        py::list argv;
        argv.append(py::str(script));
        if (!input.empty())
            argv.append(py::str(input));
        for (const std::string& argument : command_line.script_args)
            argv.append(py::str(argument));
        sys.attr("argv") = argv;

        // sys.path[0] is the script's directory, so sibling imports work exactly
        // as they do for `python script.py`.
        sys.attr("path").attr("insert")(0, py::str(boost::filesystem::path(script).parent_path().string()));

        py::module_ builtins = py::module_::import("builtins");
        // Read as bytes and let compile() honour any PEP 263 coding declaration.
        py::object source = py::module_::import("pathlib").attr("Path")(py::str(script)).attr("read_bytes")();
        py::object code   = builtins.attr("compile")(source, py::str(script), py::str("exec"));

        py::dict globals = py::module_::import("__main__").attr("__dict__");
        globals["__name__"]     = py::str("__main__");
        globals["__file__"]     = py::str(script);
        globals["__builtins__"] = builtins;
        globals["__package__"]  = py::none();
        globals["__spec__"]     = py::none();

        builtins.attr("exec")(code, globals);
    } catch (py::error_already_set& error) {
        exit_code = report_uncaught(error);
    }

    // Release script-owned objects and flush both streams before the interpreter
    // goes away, on the error path as well as the normal one.
    try {
        py::module_ sys        = py::module_::import("sys");
        py::module_ gc         = py::module_::import("gc");
        py::dict    globals    = py::module_::import("__main__").attr("__dict__");
        py::object  saved      = globals["__builtins__"];
        globals.clear();
        globals["__builtins__"] = saved;
        gc.attr("collect")();
        sys.attr("stdout").attr("flush")();
        sys.attr("stderr").attr("flush")();
    } catch (const py::error_already_set& error) {
        BOOST_LOG_TRIVIAL(warning) << "Failed to release Python state after the script: " << error.what();
    }

    host_api::disable_capability(host_api::CAP_SCRIPT_EXECUTE);
    return exit_code;
}

} // namespace Slic3r
