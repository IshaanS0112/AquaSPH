#pragma once
#include <string>
#include <vector>
#include "Scenario.hpp"

namespace aquasph {

// JSON -> Scenario. The whole point of the architecture: a new phenomenon is a new file here
// plus existing primitives, never new solver code.
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

    // Directories searched for `--scenario`, so it works from both the repo root and build/.
    static std::vector<std::string> defaultSearchDirs();
};

} // namespace aquasph
