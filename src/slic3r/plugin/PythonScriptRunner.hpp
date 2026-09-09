#ifndef slic3r_PythonScriptRunner_hpp_
#define slic3r_PythonScriptRunner_hpp_

// Script mode: run exactly one user-supplied Python script inside the Orca
// process and exit, without starting a GUI.
//
//   OrcaSlicer --file assembly.3mf --script inspect.py -- --compact
//   OrcaSlicer assembly.3mf --script inspect.py
//
// Parsing and execution are separated so both halves can be tested: the parser
// is pure, and the runner only ever sees resolved paths.
// See docs/backlog/python-api-contract.md for the normative contract.

#include <string>
#include <vector>

namespace Slic3r {

// Exit codes of script mode. The existing CLI's codes are untouched.
enum ScriptExitCode {
    SCRIPT_EXIT_OK              = 0,   // normal completion, help, or SystemExit(None)
    SCRIPT_EXIT_ERROR           = 1,   // uncaught Python exception
    SCRIPT_EXIT_INVALID_ARGS    = 2,   // bad application arguments; script not executed
    SCRIPT_EXIT_RUNTIME_FAILED  = 3,   // interpreter or orca module unavailable; script not executed
    SCRIPT_EXIT_IO              = 4,   // preflight I/O failure, or uncaught OSError/ProjectReadError
    SCRIPT_EXIT_INTERRUPTED     = 130, // uncaught KeyboardInterrupt
};

struct ScriptCommandLine
{
    // True when --script appeared before the first "--". Only then does script
    // mode apply; otherwise the existing CLI keeps its behaviour unchanged.
    bool                     script_mode = false;
    // Set when --help appeared before the delimiter: print help and exit 0.
    bool                     help_requested = false;
    // Non-empty when parsing failed. The script is not executed; exit code 2.
    std::string              error;
    std::string              script_path;  // as written on the command line
    std::string              input_path;   // as written; empty when no input was given
    std::vector<std::string> script_args;  // everything after the first "--", verbatim
};

// Parse argv for script mode. Application parsing stops at the first "--";
// every later token is forwarded to the script untouched, even when it looks
// like an application option.
ScriptCommandLine parse_script_command_line(int argc, char** argv);

// Run the script named by an already parsed command line.
//
// Resolves the script and optional input against the invocation working
// directory without changing it, checks both are readable regular files, brings
// up the bundled interpreter without a wx application, and executes the file as
// __main__ with sys.argv = [script, *input, *script_args].
// Returns one of the exit codes above.
int run_python_script(const ScriptCommandLine& command_line);

} // namespace Slic3r

#endif /* slic3r_PythonScriptRunner_hpp_ */
