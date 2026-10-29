#pragma once

// IES photometric profiles (ANSI/IES LM-63-1995, -2002 and -2019 files) for
// UsdLuxShapingAPI's shaping:ies:file. A profile is resampled to a regular
// grid that the light table stores for the integrator (src/device/light_table.h,
// shaders/integrator/lights.h: iesLookup):
//
//   rows    kIesRows vertical angles theta = 180 * j / (kIesRows - 1) degrees,
//           0 the profile's nadir: the light's local -Z, its primary axis;
//   columns `width` horizontal angles phi = 360 * i / width degrees (periodic),
//           0 along the light's local +X, 90 along +Y; width 1 for a profile
//           symmetric about its axis.
//
// The grid holds intensities divided by the profile's peak (`peak`, in
// candela after the file's candela multiplier), so its values lie in [0, 1].
// Between the file's angles intensities are interpolated linearly; outside
// its vertical range they are zero (reached linearly over one grid step past
// its last angle, when that is below 180 degrees).

#include "../internal_export.h"
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace prism {

struct PRISM_INTERNAL_API IesProfile {
    static constexpr int kIesRows = 181;        // 1 degree steps
    static constexpr int kIesColumns = 360;     // 1 degree steps, unless symmetric
    int width = 1;
    int height = kIesRows;
    std::vector<float> grid;     // height rows of width values, row-major
    float peak = 0.0f;           // candela of grid value 1
    float mean = 0.0f;           // mean candela over the sphere: total power / (4 pi)
    std::string source;          // file name, for messages

    // The grid's value at (theta, phi) in radians: bilinear, as the
    // integrator interpolates it (iesLookup). theta outside [0, pi]: 0.
    float sample(float theta, float phi) const;
};

// Parses an LM-63 profile. Throws std::runtime_error on malformed files.
PRISM_INTERNAL_API IesProfile ParseIesProfile(std::string const& text, std::string const& source = {});

// Loads a profile; on failure prints one warning per file (per process) and
// returns null, so the light renders without its profile.
PRISM_INTERNAL_API std::shared_ptr<const IesProfile> LoadIesProfile(std::filesystem::path const& path);

// Warns once per authored path that a profile could not be resolved.
PRISM_INTERNAL_API void WarnUnresolvedIesProfile(std::string const& assetPath);

} // namespace prism
