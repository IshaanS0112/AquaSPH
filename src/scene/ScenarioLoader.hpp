#pragma once
#include <string>
#include <vector>
#include "Scenario.hpp"

namespace aquasph {

// JSON -> Scenario. The whole point of the architecture: a new phenomenon
// is a new file here plus existing primitives, never new solver code.
//
// Error policy: unknown keys are ignored (forward compatibility), missing
// keys take the documented default, and a malformed *value* (a shape with
// no type, a material name that is not defined) is reported on stderr and
// falls back rather than throwing. A scenario that half-loads and runs
// something other than what was written is worse than one that says what
// it substituted, so every substitution prints.
class ScenarioLoader {
public:
    // Loads `path` directly.
    static bool loadFile(const std::string& path, Scenario& out, std::string& error);

    // Resolves a bare scenario name against the search directories, in
    // order, trying "<dir>/<name>.json". Returns the path that was used.
    static std::string resolve(const std::string& name,
                                const std::vector<std::string>& searchDirs);

    // Every *.json in the search directories, name-sorted, deduplicated by
    // basename with earlier directories winning.
    static std::vector<std::string> listAvailable(const std::vector<std::string>& searchDirs);

    // The directories `--scenario` searches, relative to the process's
    // working directory: alongside the binary first (CMake copies
    // configs/ into the build tree), then the source tree, so both
    // `cd build && ./aquasph` and `./build/aquasph` from the repo root
    // work without a flag.
    static std::vector<std::string> defaultSearchDirs();
};

} // namespace aquasph
