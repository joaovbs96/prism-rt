// The MaterialX version check: silent inside the tested range, a warning
// outside it, and at most one warning per process.

#include "../src/materialx/loader.h"
#include <iostream>
#include <sstream>
#include <stdexcept>

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    using L = DocumentLoader;
    const int* lo = L::kTestedMaterialXMin;
    const int* hi = L::kTestedMaterialXMax;

    require(L::materialXVersionWarning(lo[0], lo[1], lo[2]).empty(), "range minimum is accepted");
    require(L::materialXVersionWarning(hi[0], hi[1], hi[2]).empty(), "range maximum is accepted");
    require(L::materialXVersionWarning(1, 39, 3).empty(), "1.39.3 is inside the range");
    require(L::materialXVersionWarning(1, 39, 4).empty(), "1.39.4 is inside the range");
    require(!L::materialXVersionWarning(1, 39, 2).empty(), "1.39.2 is outside the range");
    require(!L::materialXVersionWarning(1, 38, 9).empty(), "1.38.9 is outside the range");
    require(!L::materialXVersionWarning(1, 40, 0).empty(), "1.40.0 is outside the range");
    require(!L::materialXVersionWarning(2, 0, 0).empty(), "2.0.0 is outside the range");
    const std::string text = L::materialXVersionWarning(1, 39, 2);
    require(text.find("1.39.2") != std::string::npos, "the warning names the version");
    require(text.find("[MaterialX] WARNING") != std::string::npos, "the warning has the usual prefix");

    // Once per process: the linked library either warns the first time and
    // never again, or never warns.
    std::ostringstream captured;
    auto* previous = std::cerr.rdbuf(captured.rdbuf());
    const bool first = L::warnOnceIfUntestedMaterialX();
    const bool second = L::warnOnceIfUntestedMaterialX();
    std::cerr.rdbuf(previous);
    require(!second, "the check warns at most once");
    require(first == !captured.str().empty(), "the first call prints exactly when it reports a warning");
    std::cout << "materialx_loader_version: ok\n";
    return 0;
}
