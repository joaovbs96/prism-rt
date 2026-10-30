// Windows' 260-character path limit: the pure decision behind the start-up
// check (src/renderer/install_paths.h). The check itself reads a file written
// by the packaging scripts and is exercised by packaging, not here.
#include "../src/renderer/install_paths.h"

#include <iostream>
#include <string>

namespace {
int failures = 0;
void check(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}
bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
} // namespace

int main()
{
    using prism::InstallPathProblem;
    using prism::ParseInstallPaths;

    // A short root with a deep resource fits.
    check(InstallPathProblem("C:/PrismRT", 120, false).empty(), "short root is fine");
    // Exactly the limit fits; one more does not.
    std::string root = "C:/" + std::string(100, 'a');
    std::size_t const limit = prism::kWindowsPathLimit;
    check(InstallPathProblem(root, limit - root.size(), false).empty(), "259 characters fit");
    check(!InstallPathProblem(root, limit - root.size() + 1, false).empty(), "260 characters do not");

    // A long fake root fails with one message that names the fixes.
    std::string longRoot = "C:/" + std::string(200, 'd') + "/PrismRT";
    std::string const message = InstallPathProblem(longRoot, 130, false);
    check(!message.empty(), "long root is refused");
    check(contains(message, "too long"), "message says too long");
    check(contains(message, "C:\\PrismRT"), "message suggests a shorter folder");
    check(contains(message, "LongPathsEnabled"), "message names the long paths setting");
    check(contains(message, std::to_string(longRoot.size() + 130)), "message gives the length");

    // Long paths available: never a problem.
    check(InstallPathProblem(longRoot, 130, true).empty(), "long paths enabled");

    // prism_paths.txt.
    prism::InstallPaths paths;
    check(ParseInstallPaths("root=..\r\nrequired=117\r\nall=140\r\n", "C:/x/PrismRT/bin", paths), "parse");
    check(paths.required == 117 && paths.all == 140, "parsed lengths");
    check(paths.root.generic_string() == "C:/x/PrismRT",
          "root is the bin directory's parent: " + paths.root.generic_string());
    check(ParseInstallPaths("required=50\n", "C:/x/bin", paths) && paths.all == 50, "all defaults to required");
    check(!ParseInstallPaths("all=50\n", "C:/x/bin", paths), "required is mandatory");
    check(!ParseInstallPaths("required=abc\n", "C:/x/bin", paths), "bad number");
    check(ParseInstallPaths("root=../..\nrequired=9\n", "C:/x/site/prismrt/bin", paths) &&
              paths.root.generic_string() == "C:/x/site",
          "wheel root two levels up");

    // A build tree (no prism_paths.txt beside prism.dll) gives no problem.
    check(prism::CheckInstallPathLength().empty(), "no marker, no check");

    if (failures) return 1;
    std::cout << "install_paths_test: ok\n";
    return 0;
}
