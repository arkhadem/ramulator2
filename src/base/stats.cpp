#include "base/stats.h"

namespace Ramulator {

YAML::Emitter& operator<<(YAML::Emitter& emitter, const Stats& s) {
    // collect keys
    std::vector<std::string> keys;
    keys.reserve(s._registry.size());
    for (auto& kv : s._registry) {
        keys.push_back(kv.first);
    }

    // sort keys alphabetically
    std::sort(keys.begin(), keys.end());

    // print in sorted order
    for (auto& key : keys) {
        auto* stat = s._registry.at(key);
        stat->emit_to(emitter);
    }
    return emitter;
}


}        // namespace Ramulator
