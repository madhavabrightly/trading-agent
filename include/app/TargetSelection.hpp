#pragma once

// TargetSelection — exact target picker. When the requested URL does not match
// an open page (TARGET_NOT_FOUND), enumerate open page targets and let the user
// select one. The durable result is a URL RULE (exact / origin-path), never a
// CDP target id (CDP ids are ephemeral across Edge restarts).

#include "core/Types.hpp"
#include <string>
#include <vector>
#include <optional>

namespace edgemon {

struct SelectedTarget {
    EdgeTarget target;                  // the chosen open page (live CDP info)
    PageUrl ruleUrl;                    // URL to persist + re-resolve by
    TargetMatchRule rule = TargetMatchRule::Exact;
};

class TargetSelection {
public:
    // Formats the spec-style picker table for the open pages:
    //   [1] ChatGPT — https://chatgpt.com/c/...
    //   [2] Wikipedia — https://www.wikipedia.org/
    static std::string formatList(const std::vector<EdgeTarget>& pages);

    // Interactive CLI prompt over std::cin. Returns nullopt on EOF / invalid.
    static std::optional<SelectedTarget> promptCli(
        const std::vector<EdgeTarget>& pages);

    // Chooses a sensible default rule for a selected page URL:
    // chatgpt.com/c/<id> style dynamic SPA conversations => origin-path,
    // otherwise exact. Kept deterministic and explicit.
    static TargetMatchRule defaultRuleFor(const std::string& pageUrl);

    // Converts the rule enum to/from its persisted string form.
    static std::optional<TargetMatchRule> ruleFromString(const std::string& s);
};

} // namespace edgemon
