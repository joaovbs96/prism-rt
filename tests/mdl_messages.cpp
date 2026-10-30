// How MDL compiler messages reach the log (src/mdl/messages.h): warnings about
// unused generated code (C181, C183, C350) are verbose output only, each
// message is printed once (the SDK's receiving logger and the execution
// context both carry the core compiler's messages), and a texture published
// next to its document ("/<name>" in the module) resolves through the MDL
// search paths, without C295 ("only found via the resource search paths").

#include "../src/mdl/context.h"
#include "../src/mdl/material_compiler.h"
#include "../src/mdl/messages.h"
#include "../src/materialx/mdl_generator.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

int failures = 0;

void Expect(bool condition, std::string const& what)
{
    if (condition) return;
    std::cerr << "FAILED: " << what << "\n";
    ++failures;
}

struct ScratchDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("prism-mdl-messages-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ScratchDirectory() { std::filesystem::create_directories(path); }
    ~ScratchDirectory() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};

// Standard output and error while it lives.
struct Capture {
    std::ostringstream text;
    std::streambuf* err = std::cerr.rdbuf(text.rdbuf());
    std::streambuf* out = std::cout.rdbuf(text.rdbuf());
    ~Capture() { restore(); }
    std::string restore()
    {
        if (err) std::cerr.rdbuf(err);
        if (out) std::cout.rdbuf(out);
        err = out = nullptr;
        return text.str();
    }
};

std::string MdlLibraries()
{
    if (const char* configured = std::getenv("PRISM_MATERIALX_MDL"); configured && *configured) return configured;
    if (const char* libraries = std::getenv("PRISM_MATERIALX_LIBRARY_PATH"); libraries && *libraries)
        return (std::filesystem::path(libraries) / "mdl").string();
    return {};
}

GeneratedMDL Module(std::string const& name, std::string const& body, std::string const& texture)
{
    GeneratedMDL generated;
    generated.module_name = "::" + name;
    generated.texture_paths = {texture};
    generated.success = true;
    generated.mdl_source =
        "mdl 1.7;\n"
        "import ::df::*;\n"
        "import ::tex::*;\n"
        "import ::state::*;\n"
        "float helper(float x) {\n"
        "    float unused_value = 2.0;\n"   // C181 unused variable
        "    return x" + body + ";\n"
        "}\n"
        "export material main(uniform texture_2d image = texture_2d(\"" + texture + "\")) = material(\n"
        "  surface: material_surface(scattering: df::diffuse_reflection_bsdf(\n"
        "    tint: tex::lookup_color(image, float2(helper(state::texture_coordinate(0).x), "
        "state::texture_coordinate(0).y)))));\n";
    return generated;
}

// A message without the logger's or the context printer's prefix.
std::string MessageText(std::string const& line)
{
    const size_t source = line.find("<string>(");
    return source == std::string::npos ? line : line.substr(source);
}

} // namespace

int main()
{
    // The rules on their own.
    Expect(mdl::IsUnusedCodeMessage("<string>(46,12): C181 unused variable 'geomprop_UV0_out1'"), "C181 is unused code");
    Expect(mdl::IsUnusedCodeMessage("warn : <string>(707,15): C350 unused let temporary 'c_box_filter_weights'"),
           "C350 is unused code");
    Expect(mdl::IsUnusedCodeMessage("<string>(257,9): C183 unused parameter 'style'"), "C183 is unused code");
    Expect(!mdl::IsUnusedCodeMessage("C295 the file path '/a.exr' is only found via the resource search paths"),
           "C295 is not unused code");
    Expect(!mdl::IsUnusedCodeMessage("<string>(3,1): C102 'x' has not been declared"), "an error is not unused code");
    Expect(mdl::HasSearchPathTexture({"63E3500D9B820CC0E0817201F340CD1A1F0208E3.exr"}),
           "a published texture's file name resolves through the search paths");
    Expect(mdl::HasSearchPathTexture({"/red.ppm"}), "an absolute MDL path resolves through the search paths");
    Expect(!mdl::HasSearchPathTexture({"C:/textures/red.ppm"}), "a drive path names one file");
    Expect(!mdl::HasSearchPathTexture({}), "no textures");

    try {
        ScratchDirectory scratch;
        {
            std::ofstream image(scratch.path / "red.ppm", std::ios::binary);
            image << "P6\n2 2\n255\n";
            for (int i = 0; i != 4; ++i) image.write("\xff\x08\x04", 3);
        }
        mdl::Context context;
        if (!context.is_valid()) throw std::runtime_error("cannot initialize MDL SDK: " + context.get_error());
        mdl::MaterialCompiler compiler(context, MdlLibraries());
        const std::string document = (scratch.path / "fixture.mtlx").string();

        // A module with an unused variable and a texture next to its document.
        {
            Capture capture;
            auto compiled = compiler.compile(Module("messages_ok", "", "/red.ppm"), document);
            const bool success = compiled.success;
            const std::string error = compiled.error_message;
            compiled.release();
            const std::string log = capture.restore();
            Expect(success, "the module compiles: " + error);
            Expect(log.find("C181") == std::string::npos, "C181 is not printed by default:\n" + log);
            Expect(log.find("C295") == std::string::npos, "the texture resolves without C295:\n" + log);
            Expect(log.find("unused") == std::string::npos, "no unused-code warning is printed:\n" + log);
        }

        // An error is printed, once.
        {
            Capture capture;
            auto compiled = compiler.compile(Module("messages_error", " + undeclared_symbol", "/red.ppm"), document);
            const bool success = compiled.success;
            compiled.release();
            const std::string log = capture.restore();
            Expect(!success, "a module with an undeclared symbol fails");
            std::map<std::string, int> counts;
            std::istringstream lines(log);
            for (std::string line; std::getline(lines, line);)
                if (line.find("undeclared_symbol") != std::string::npos) ++counts[MessageText(line)];
            Expect(!counts.empty(), "the error is printed:\n" + log);
            for (auto const& [message, count] : counts)
                Expect(count == 1, "printed once (" + std::to_string(count) + "x): " + message + "\n" + log);
        }
    } catch (std::exception const& error) {
        std::cerr << "FAILED: " << error.what() << "\n";
        return 1;
    }
    if (failures) return 1;
    std::cout << "mdl messages: OK\n";
    return 0;
}
