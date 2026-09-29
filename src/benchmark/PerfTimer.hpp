#pragma once
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>
#include <numeric>

namespace aquasph {

// Minimal wall-clock timing utility: start/stop a named label repeatedly (once per simulation
// step) and query rolling average / last / count.
class PerfTimer {
public:
    void start(const std::string& label) {
        starts_[label] = std::chrono::high_resolution_clock::now();
    }

    void stop(const std::string& label) {
        const auto end = std::chrono::high_resolution_clock::now();
        auto it = starts_.find(label);
        if (it == starts_.end()) return;
        const double ms = std::chrono::duration<double, std::milli>(end - it->second).count();
        history_[label].push_back(ms);
    }

    double avgTimeMs(const std::string& label) const {
        auto it = history_.find(label);
        if (it == history_.end() || it->second.empty()) return 0.0;
        const auto& v = it->second;
        return std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
    }

    double lastTimeMs(const std::string& label) const {
        auto it = history_.find(label);
        if (it == history_.end() || it->second.empty()) return 0.0;
        return it->second.back();
    }

    size_t sampleCount(const std::string& label) const {
        auto it = history_.find(label);
        return it == history_.end() ? 0 : it->second.size();
    }

private:
    std::unordered_map<std::string, std::chrono::high_resolution_clock::time_point> starts_;
    std::unordered_map<std::string, std::vector<double>> history_;
};

} // namespace aquasph
