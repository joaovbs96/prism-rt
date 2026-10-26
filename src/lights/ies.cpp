#include "ies.h"

#include "../device/hlsl_math.h"
#include "../../shaders/integrator/lights.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>

namespace prism {

namespace {

constexpr double kPiD = 3.14159265358979323846;

// The interval [x[i], x[i + 1]] of a sorted table holding `at` (clamped to
// the table) and the position t in it.
void Locate(std::vector<double> const& x, double at, int* index, double* t)
{
    const auto upper = std::upper_bound(x.begin(), x.end(), at);
    const int i = std::clamp(int(upper - x.begin()) - 1, 0, int(x.size()) - 2);
    *index = i;
    const double span = x[size_t(i) + 1] - x[size_t(i)];
    *t = span > 0.0 ? std::clamp((at - x[size_t(i)]) / span, 0.0, 1.0) : 0.0;
}

struct Photometry {
    std::vector<double> vertical;     // degrees, ascending
    std::vector<double> horizontal;   // degrees, ascending
    std::vector<double> candela;      // [h * vertical.size() + v], after the multiplier

    double value(int h, int v) const { return candela[size_t(h) * vertical.size() + size_t(v)]; }

    // Intensity along vertical angle theta at the file's horizontal angle h.
    double alongVertical(int h, double theta) const
    {
        if (vertical.size() == 1) return theta == vertical[0] ? value(h, 0) : 0.0;
        if (theta < vertical.front() || theta > vertical.back()) return 0.0;
        int i;
        double t;
        Locate(vertical, theta, &i, &t);
        return value(h, i) * (1.0 - t) + value(h, i + 1) * t;
    }

    // Type C symmetry (LM-63-2019 section 5.10 / 5.11): the horizontal angle
    // phi in [0, 360) mapped into the file's range.
    double intensity(double theta, double phi) const
    {
        const size_t n = horizontal.size();
        if (n == 1) return alongVertical(0, theta);
        const double first = horizontal.front(), last = horizontal.back();
        if (first == 0.0 && last == 90.0) {          // quadrant symmetric
            phi = std::fmod(phi, 180.0);
            if (phi > 90.0) phi = 180.0 - phi;
        } else if (first == 0.0 && last == 180.0) {  // symmetric about the 0-180 plane
            if (phi > 180.0) phi = 360.0 - phi;
        } else if (first == 90.0 && last == 270.0) { // symmetric about the 90-270 plane
            if (phi < 90.0) phi = 180.0 - phi;
            else if (phi > 270.0) phi = 540.0 - phi;
        }
        // Full (0 to 360, or an incomplete range): periodic over 360 degrees.
        if (phi < first) phi += 360.0;
        if (phi <= last) {
            int i;
            double t;
            Locate(horizontal, phi, &i, &t);
            return alongVertical(i, theta) * (1.0 - t) + alongVertical(i + 1, theta) * t;
        }
        // Between the last angle and the first one, 360 degrees later.
        const double span = first + 360.0 - last;
        const double t = span > 0.0 ? (phi - last) / span : 0.0;
        return alongVertical(int(n) - 1, theta) * (1.0 - t) + alongVertical(0, theta) * t;
    }

    bool rotationallySymmetric() const
    {
        if (horizontal.size() == 1) return true;
        for (size_t h = 1; h < horizontal.size(); ++h)
            for (size_t v = 0; v < vertical.size(); ++v)
                if (value(int(h), int(v)) != value(0, int(v))) return false;
        return true;
    }
};

// The file's numbers after TILT, separated by white space or commas.
class Numbers {
public:
    explicit Numbers(std::string text) : m_stream((std::replace(text.begin(), text.end(), ',', ' '), text)) {}
    double take(char const* what)
    {
        std::string token;
        if (!(m_stream >> token))
            throw std::runtime_error(std::string("unexpected end of file reading ") + what);
        char* end = nullptr;
        const double value = std::strtod(token.c_str(), &end);
        if (end == token.c_str() || *end != '\0' || !std::isfinite(value))
            throw std::runtime_error(std::string("invalid number '") + token + "' reading " + what);
        return value;
    }

private:
    std::istringstream m_stream;
};

std::string Trimmed(std::string s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    return s.substr(start);
}

void WarnOnce(std::string const& key, std::string const& message)
{
    static std::mutex mutex;
    static std::set<std::string> warned;
    std::lock_guard lock(mutex);
    if (warned.insert(key).second) std::cerr << message << '\n';
}

} // namespace

float IesProfile::sample(float theta, float phi) const
{
    const IesLookup l = iesGridLookup(theta, phi, uint(width), uint(height));
    if (!l.valid) return 0.0f;
    auto at = [&](uint i, uint j) { return grid[size_t(j) * size_t(width) + i]; };
    const float top = lerp(at(l.i0, l.j0), at(l.i1, l.j0), l.tx);
    const float bottom = lerp(at(l.i0, l.j0 + 1u), at(l.i1, l.j0 + 1u), l.tx);
    return lerp(top, bottom, l.ty);
}

IesProfile ParseIesProfile(std::string const& text, std::string const& source)
{
    // Header and keywords up to the TILT line.
    std::istringstream lines(text);
    std::string line;
    std::string tilt;
    bool foundTilt = false;
    while (std::getline(lines, line)) {
        const std::string trimmed = Trimmed(line);
        if (trimmed.rfind("TILT", 0) == 0) {
            const size_t equals = trimmed.find('=');
            tilt = equals == std::string::npos ? std::string() : Trimmed(trimmed.substr(equals + 1));
            foundTilt = true;
            break;
        }
    }
    if (!foundTilt) throw std::runtime_error("no TILT line");
    std::string rest((std::istreambuf_iterator<char>(lines)), std::istreambuf_iterator<char>());
    Numbers numbers(rest);
    if (tilt == "INCLUDE") {
        // Lamp-to-luminaire geometry, then the tilt angles and factors: the
        // tilt describes the lamp's output by its inclination, which a USD
        // light does not have. Skipped.
        numbers.take("tilt geometry");
        const double count = numbers.take("tilt angle count");
        if (count < 0 || count > 100000) throw std::runtime_error("invalid tilt angle count");
        for (int i = 0; i < 2 * int(count); ++i) numbers.take("tilt values");
    }
    numbers.take("number of lamps");
    numbers.take("lumens per lamp");
    const double multiplier = numbers.take("candela multiplier");
    const double verticalCount = numbers.take("number of vertical angles");
    const double horizontalCount = numbers.take("number of horizontal angles");
    const double photometricType = numbers.take("photometric type");
    numbers.take("units type");
    numbers.take("width");
    numbers.take("length");
    numbers.take("height");
    numbers.take("ballast factor");
    numbers.take("file generation type");
    numbers.take("input watts");
    if (verticalCount < 1 || verticalCount > 100000 || horizontalCount < 1 || horizontalCount > 100000 ||
        verticalCount != std::floor(verticalCount) || horizontalCount != std::floor(horizontalCount))
        throw std::runtime_error("invalid angle counts");
    if (photometricType != 1.0)
        WarnOnce("type:" + source, "PrismRT: IES profile '" + source + "' has photometric type " +
                                       std::to_string(int(photometricType)) +
                                       " (not C); reading its angles as type C");
    Photometry p;
    p.vertical.resize(size_t(verticalCount));
    p.horizontal.resize(size_t(horizontalCount));
    for (double& v : p.vertical) v = numbers.take("vertical angles");
    for (double& h : p.horizontal) h = numbers.take("horizontal angles");
    p.candela.resize(p.vertical.size() * p.horizontal.size());
    for (double& c : p.candela) c = std::max(0.0, numbers.take("candela values") * multiplier);
    if (!std::is_sorted(p.vertical.begin(), p.vertical.end()) ||
        !std::is_sorted(p.horizontal.begin(), p.horizontal.end()))
        throw std::runtime_error("angles are not in increasing order");
    if (p.vertical.front() < 0.0 || p.vertical.back() > 180.0)
        throw std::runtime_error("vertical angles outside [0, 180]");

    IesProfile profile;
    profile.source = source;
    profile.width = p.rotationallySymmetric() ? 1 : IesProfile::kIesColumns;
    profile.height = IesProfile::kIesRows;
    std::vector<double> grid(size_t(profile.width) * size_t(profile.height));
    double peak = 0.0;
    for (int j = 0; j < profile.height; ++j) {
        const double theta = 180.0 * double(j) / double(profile.height - 1);
        for (int i = 0; i < profile.width; ++i) {
            const double phi = 360.0 * double(i) / double(profile.width);
            const double value = p.intensity(theta, phi);
            grid[size_t(j) * size_t(profile.width) + size_t(i)] = value;
            peak = std::max(peak, value);
        }
    }
    profile.peak = float(peak);
    profile.grid.resize(grid.size());
    for (size_t k = 0; k < grid.size(); ++k) profile.grid[k] = peak > 0.0 ? float(grid[k] / peak) : 0.0f;

    // Total power / 4 pi of the interpolated grid: the mean over the sphere
    // (midpoint rule in theta, the grid's bilinear interpolant).
    constexpr int kThetaSteps = 720;
    const int phiSteps = profile.width == 1 ? 1 : 4 * profile.width;
    double sum = 0.0;
    for (int t = 0; t < kThetaSteps; ++t) {
        const double theta = kPiD * (double(t) + 0.5) / double(kThetaSteps);
        double ring = 0.0;
        for (int f = 0; f < phiSteps; ++f) {
            const double phi = 2.0 * kPiD * (double(f) + 0.5) / double(phiSteps);
            ring += profile.sample(float(theta), float(phi));
        }
        sum += ring / double(phiSteps) * std::sin(theta) * (kPiD / double(kThetaSteps));
    }
    profile.mean = float(0.5 * sum * peak); // (2 pi * integral) / (4 pi)
    return profile;
}

std::shared_ptr<const IesProfile> LoadIesProfile(std::filesystem::path const& path)
{
    const std::string name = path.string();
    try {
        std::ifstream file(path, std::ios::binary);
        if (!file) throw std::runtime_error("cannot open the file");
        std::stringstream text;
        text << file.rdbuf();
        auto profile = std::make_shared<IesProfile>(ParseIesProfile(text.str(), name));
        if (!(profile->peak > 0.0f))
            WarnOnce("zero:" + name, "PrismRT: IES profile '" + name + "' has no positive intensity; "
                                     "its light is dark");
        return profile;
    } catch (std::exception const& e) {
        WarnOnce("invalid:" + name, "PrismRT: could not read IES profile '" + name + "' (" + e.what() +
                                        "); the light renders without it");
        return nullptr;
    }
}

void WarnUnresolvedIesProfile(std::string const& assetPath)
{
    WarnOnce("unresolved:" + assetPath, "PrismRT: could not resolve IES profile '" + assetPath +
                                            "'; the light renders without it");
}

} // namespace prism
