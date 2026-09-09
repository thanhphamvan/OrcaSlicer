#include <catch2/catch_all.hpp>

#include <slic3r/plugin/PythonScriptRunner.hpp>

#include <string>
#include <vector>

using namespace Slic3r;

namespace {

// Catch2 owns the strings; parse_script_command_line only reads them.
ScriptCommandLine parse(std::vector<std::string> tokens)
{
    std::vector<char*> argv;
    argv.reserve(tokens.size() + 1);
    for (std::string& token : tokens)
        argv.push_back(token.data());
    argv.push_back(nullptr);
    return parse_script_command_line(static_cast<int>(tokens.size()), argv.data());
}

} // namespace

TEST_CASE("Script mode accepts named and positional model inputs alike", "[ScriptCommandLine][Python]")
{
    const ScriptCommandLine named = parse({"OrcaSlicer", "--file", "assembly.3mf", "--script", "inspect.py"});
    const ScriptCommandLine positional = parse({"OrcaSlicer", "assembly.3mf", "--script", "inspect.py"});

    for (const ScriptCommandLine& parsed : {named, positional}) {
        CHECK(parsed.script_mode);
        CHECK(parsed.error.empty());
        CHECK_FALSE(parsed.help_requested);
        CHECK(parsed.script_path == "inspect.py");
        CHECK(parsed.input_path == "assembly.3mf");
        CHECK(parsed.script_args.empty());
    }
}

TEST_CASE("Script mode option order and --key=value forms do not matter", "[ScriptCommandLine][Python]")
{
    const ScriptCommandLine parsed = parse({"OrcaSlicer", "--script=inspect.py", "--file=assembly.3mf"});
    CHECK(parsed.script_mode);
    CHECK(parsed.error.empty());
    CHECK(parsed.script_path == "inspect.py");
    CHECK(parsed.input_path == "assembly.3mf");
}

TEST_CASE("Everything after the first delimiter reaches the script unchanged", "[ScriptCommandLine][Python]")
{
    const ScriptCommandLine parsed = parse({"OrcaSlicer", "--script", "inspect.py", "--",
                                            "--compact", "--script", "not-a-script.py", "--", "--file"});
    CHECK(parsed.script_mode);
    CHECK(parsed.error.empty());
    CHECK(parsed.script_path == "inspect.py");
    CHECK(parsed.input_path.empty());
    CHECK(parsed.script_args == std::vector<std::string>{"--compact", "--script", "not-a-script.py", "--", "--file"});
}

TEST_CASE("Script mode is detected only before the delimiter", "[ScriptCommandLine][Python]")
{
    // A trailing --script belongs to whatever the existing CLI makes of it.
    const ScriptCommandLine parsed = parse({"OrcaSlicer", "model.3mf", "--", "--script", "inspect.py"});
    CHECK_FALSE(parsed.script_mode);
    CHECK(parsed.error.empty());
    CHECK(parsed.script_path.empty());
    CHECK(parsed.script_args.empty());
}

TEST_CASE("Script mode rejects ambiguous and unsupported command lines", "[ScriptCommandLine][Python]")
{
    const std::vector<std::vector<std::string>> invalid = {
        {"OrcaSlicer", "--script", "a.py", "--script", "b.py"},
        {"OrcaSlicer", "--script", "a.py", "--file", "x.3mf", "--file", "y.3mf"},
        {"OrcaSlicer", "--script", "a.py", "--file", "x.3mf", "y.3mf"},
        {"OrcaSlicer", "--script", "a.py", "x.3mf", "y.3mf"},
        {"OrcaSlicer", "--script", "a.py", "--slice", "0"},
        {"OrcaSlicer", "--script", "a.py", "--export-3mf", "out.3mf"},
        {"OrcaSlicer", "--script"},
        {"OrcaSlicer", "--script", "a.py", "--file"},
    };

    for (const std::vector<std::string>& tokens : invalid) {
        CAPTURE(tokens);
        const ScriptCommandLine parsed = parse(tokens);
        CHECK(parsed.script_mode);
        CHECK_FALSE(parsed.error.empty());
    }
}

TEST_CASE("Application help is only recognized before the delimiter", "[ScriptCommandLine][Python]")
{
    const ScriptCommandLine before = parse({"OrcaSlicer", "--script", "a.py", "--help"});
    CHECK(before.script_mode);
    CHECK(before.help_requested);
    CHECK(before.error.empty());

    const ScriptCommandLine after = parse({"OrcaSlicer", "--script", "a.py", "--", "--help"});
    CHECK(after.script_mode);
    CHECK_FALSE(after.help_requested);
    CHECK(after.script_args == std::vector<std::string>{"--help"});
}

TEST_CASE("Paths keep spaces, Unicode and leading dashes", "[ScriptCommandLine][Python]")
{
    const ScriptCommandLine parsed = parse({"OrcaSlicer", "--file=-Ger\xc3\xa4te \xc3\xbc.3mf", "--script",
                                            "user inventory \xc3\xbc.py", "--", "-Ger\xc3\xa4te"});
    CHECK(parsed.error.empty());
    CHECK(parsed.script_path == "user inventory \xc3\xbc.py");
    CHECK(parsed.input_path == "-Ger\xc3\xa4te \xc3\xbc.3mf");
    CHECK(parsed.script_args == std::vector<std::string>{"-Ger\xc3\xa4te"});
}

TEST_CASE("A command line without --script leaves the existing CLI untouched", "[ScriptCommandLine][Python]")
{
    const ScriptCommandLine parsed = parse({"OrcaSlicer", "model.3mf", "--slice", "0", "--export-3mf", "out.3mf"});
    CHECK_FALSE(parsed.script_mode);
    CHECK(parsed.error.empty());
    CHECK(parsed.input_path.empty());
    CHECK_FALSE(parsed.help_requested);
}
