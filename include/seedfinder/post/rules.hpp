#pragma once
// Post-rules: predicates applied to each simulated event. An event matches when
// every rule that is set holds. Rules come from the observation log.
#include <cstddef>
#include <vector>

namespace seedfinder {

struct PostRule {
    std::vector<int> prefix;  // known leading eliminations (1..6), may be empty or partial
    int survivor = 0;         // known surviving room (1..6), 0 = unknown
    unsigned exclude_mask = 0; // bit r set: room r is known NOT to be the survivor

    bool empty() const { return prefix.empty() && survivor == 0 && exclude_mask == 0; }
};

// The elimination list matches when it starts with the rule's prefix.
template <class E>
inline bool matches_prefix(const PostRule& r, const E* elim) {
    for (std::size_t i = 0; i < r.prefix.size(); ++i) {
        if (static_cast<int>(elim[i]) != r.prefix[i]) return false;
    }
    return true;
}

template <class E>
inline bool rule_matches(const PostRule& r, const E* elim, int survivor) {
    if (!matches_prefix(r, elim)) return false;
    if (r.survivor != 0 && survivor != r.survivor) return false;
    if (r.exclude_mask & (1u << survivor)) return false;
    return true;
}

}  // namespace seedfinder
