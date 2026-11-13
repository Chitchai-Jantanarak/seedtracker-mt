#pragma once
// Post-rules: predicates applied to each simulated event. Rules come from the
// observation log.
#include <cstddef>
#include <vector>

namespace seedtracker {

struct PostRule {
    std::vector<int> prefix;    // values seen in order from the start (1..6), may be empty or partial
    unsigned include_mask = 0;  // bit v set: v appears somewhere in the event's five values
    unsigned exclude_mask = 0;  // bit v set: v appears nowhere in the event's five values

    bool empty() const { return prefix.empty() && include_mask == 0 && exclude_mask == 0; }
};

// The event matches when its values start with the prefix and its five values
// contain every included value and no excluded one.
template <class E>
inline bool rule_matches(const PostRule& r, const E* elim) {
    for (std::size_t i = 0; i < r.prefix.size(); ++i) {
        if (static_cast<int>(elim[i]) != r.prefix[i]) return false;
    }
    unsigned seen = 0;
    for (std::size_t i = 0; i < 5; ++i) seen |= 1u << static_cast<int>(elim[i]);
    return (seen & r.include_mask) == r.include_mask && (seen & r.exclude_mask) == 0;
}

}  // namespace seedtracker
