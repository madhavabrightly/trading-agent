#include "app/TargetSelection.hpp"
#include "edge/UrlUtils.hpp"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace edgemon {

std::string TargetSelection::formatList(const std::vector<EdgeTarget>& pages) {
    std::ostringstream oss;
    oss << "\n=== Open Edge pages (select the EXACT tab to monitor) ===\n";
    if (pages.empty()) {
        oss << "No open page targets found.\n";
        return oss.str();
    }
    for (size_t i = 0; i < pages.size(); ++i) {
        const auto& t = pages[i];
        std::string title = t.title.empty() ? "(untitled)" : t.title;
        // Title may contain the URL already; keep the row compact but exact.
        oss << "[" << (i + 1) << "] " << title
            << (t.title.empty() ? "" : " — ") << t.url.value
            << "  | CDP:" << t.id.value
            << (t.processId.empty() ? "" : " | PID:" + std::to_string(t.processId.value))
            << "\n";
    }
    return oss.str();
}

TargetMatchRule TargetSelection::defaultRuleFor(const std::string& pageUrl) {
    // Dynamic per-item URLs (SPA conversations, dashboards) are better
    // re-located by origin+path so a refresh / query change keeps the target.
    auto parsed = UrlUtils::parse(pageUrl);
    if (!parsed) return TargetMatchRule::Exact;

    std::string host = parsed->host;
    // ChatGPT-style deep links and news/feed paths benefit from origin-path.
    bool dynamic = host.find("chatgpt.com") != std::string::npos ||
                   host.find("openai.com") != std::string::npos ||
                   host.find("timesnownews.com") != std::string::npos;
    return dynamic ? TargetMatchRule::OriginPath : TargetMatchRule::Exact;
}

std::optional<TargetMatchRule> TargetSelection::ruleFromString(const std::string& s) {
    std::string lower;
    for (char c : s) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (lower == "exact") return TargetMatchRule::Exact;
    if (lower == "url-prefix" || lower == "prefix") return TargetMatchRule::UrlPrefix;
    if (lower == "origin-path" || lower == "originpath") return TargetMatchRule::OriginPath;
    return std::nullopt;
}

std::optional<SelectedTarget> TargetSelection::promptCli(const std::vector<EdgeTarget>& pages) {
    if (pages.empty()) return std::nullopt;

    std::cout << formatList(pages);
    std::cout << "\nEnter the number of the tab to monitor (1-" << pages.size()
              << "), or 0 to cancel: " << std::flush;

    std::string line;
    if (!std::getline(std::cin, line)) return std::nullopt;

    int idx = 0;
    try {
        idx = std::stoi(line);
    } catch (...) {
        return std::nullopt;
    }
    if (idx < 1 || idx > static_cast<int>(pages.size())) return std::nullopt;

    SelectedTarget sel;
    sel.target = pages[static_cast<size_t>(idx - 1)];
    sel.ruleUrl = sel.target.url;
    sel.rule = defaultRuleFor(sel.target.url.value);
    return sel;
}

} // namespace edgemon
