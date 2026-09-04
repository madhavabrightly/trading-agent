#include "edge/TargetResolver.hpp"
#include "logger.hpp"

namespace edgemon {

void TargetResolver::addInstance(const EdgeInstance& instance) {
    instances_.push_back(instance);
    refreshTargets();
}

void TargetResolver::setInstances(const std::vector<EdgeInstance>& instances) {
    instances_ = instances;
    refreshTargets();
}

void TargetResolver::clearInstances() {
    instances_.clear();
    cachedTargets_.clear();
}

void TargetResolver::refreshTargets() {
    cachedTargets_.clear();
    
    for (const auto& instance : instances_) {
        auto targets = discovery_.enumerateTargets(instance);
        cachedTargets_.insert(cachedTargets_.end(), targets.begin(), targets.end());
    }
    
    LOG_DEBUG("TargetResolver: refreshed {} targets from {} instances", 
              cachedTargets_.size(), instances_.size());
}

std::vector<EdgeTarget> TargetResolver::filterPageTargets(
        const std::vector<EdgeTarget>& targets) {
    std::vector<EdgeTarget> pages;
    for (const auto& t : targets) {
        // Only real page targets are attachable. Extensions / background
        // pages / service workers / devtools are never candidates.
        if (t.type == "page") pages.push_back(t);
    }
    // Stable ordering by title for deterministic picker output.
    std::stable_sort(pages.begin(), pages.end(),
                     [](const EdgeTarget& a, const EdgeTarget& b) {
                         if (a.title != b.title) return a.title < b.title;
                         return a.url.value < b.url.value;
                     });
    return pages;
}

std::optional<CDPTargetId> TargetResolver::resolveByUrl(const PageUrl& url) {
    auto result = resolve(url);
    if (result.found) {
        return result.cdpTargetId;
    }
    return std::nullopt;
}

std::optional<CDPTargetId> TargetResolver::resolveByTitle(const std::string& title) {
    auto result = resolveTargetByTitle(title);
    if (result) {
        return result->id;
    }
    return std::nullopt;
}

std::optional<CDPTargetId> TargetResolver::resolveByProcessId(EdgeProcessId pid) {
    for (const auto& target : cachedTargets_) {
        if (target.processId == pid) {
            return target.id;
        }
    }
    
    refreshTargets();
    
    for (const auto& target : cachedTargets_) {
        if (target.processId == pid) {
            return target.id;
        }
    }
    
    return std::nullopt;
}

std::optional<EdgeTarget> TargetResolver::resolveTargetByUrl(const PageUrl& url) {
    auto result = resolve(url, TargetMatchRule::Exact);
    if (result.found) {
        return result.target;
    }
    return std::nullopt;
}

std::optional<EdgeTarget> TargetResolver::resolveTargetByTitle(const std::string& title) {
    for (const auto& target : cachedTargets_) {
        if (target.title.find(title) != std::string::npos) {
            return target;
        }
    }
    
    refreshTargets();
    
    for (const auto& target : cachedTargets_) {
        if (target.title.find(title) != std::string::npos) {
            return target;
        }
    }
    
    return std::nullopt;
}

TargetResolver::ResolutionResult TargetResolver::resolve(const PageUrl& url) {
    return resolveImpl(url, TargetMatchRule::Exact);
}

TargetResolver::ResolutionResult TargetResolver::resolve(const PageUrl& url,
                                                         TargetMatchRule rule) {
    return resolveImpl(url, rule);
}

TargetResolver::ResolutionResult TargetResolver::resolveImpl(const PageUrl& url,
                                                             TargetMatchRule rule) {
    ResolutionResult result;
    result.rule = rule;
    result.notFound = true;

    const std::string requested = url.value;
    LOG_INFO("TargetResolver: requested={} rule={}", requested,
             TargetMatchRuleToString(rule));

    std::vector<EdgeTarget> candidates;
    for (const auto& instance : instances_) {
        auto targets = discovery_.enumerateTargets(instance);
        candidates.insert(candidates.end(), targets.begin(), targets.end());
    }

    auto pages = filterPageTargets(candidates);
    LOG_INFO("TargetResolver: candidate_count={}", pages.size());

    for (const auto& target : pages) {
        if (UrlUtils::matches(requested, target.url.value, rule)) {
            result.found = true;
            result.notFound = false;
            result.cdpTargetId = target.id;
            result.target = target;
            auto instance = findInstanceForTarget(target);
            if (instance) {
                result.instance = *instance;
            }
            LOG_INFO("TargetResolver: selected_cdp_target={} selected_url={} (title={})",
                     target.id.value, target.url.value, target.title);
            return result;
        }
    }

    // Second pass over the cache covers targets already enumerated by an
    // earlier refresh (instances may have changed between calls).
    for (const auto& target : cachedTargets_) {
        if (target.type != "page") continue;
        if (UrlUtils::matches(requested, target.url.value, rule)) {
            result.found = true;
            result.notFound = false;
            result.cdpTargetId = target.id;
            result.target = target;
            auto instance = findInstanceForTarget(target);
            if (instance) {
                result.instance = *instance;
            }
            LOG_INFO("TargetResolver: selected_cdp_target={} selected_url={} (cached)",
                     target.id.value, target.url.value);
            return result;
        }
    }

    result.errorMessage = "TARGET_NOT_FOUND: no open page matches '" + requested +
                          "' (rule=" + TargetMatchRuleToString(rule) + ")";
    LOG_WARN("TargetResolver: {}", result.errorMessage);
    return result;
}

TargetResolver::ResolutionResult TargetResolver::resolveByExactMatch(const PageUrl& url) {
    return resolveImpl(url, TargetMatchRule::Exact);
}

std::vector<EdgeTarget> TargetResolver::getAllTargets() {
    refreshTargets();
    return cachedTargets_;
}

std::vector<EdgeInstance> TargetResolver::getInstances() {
    return instances_;
}

std::optional<EdgeInstance> TargetResolver::findInstanceForTarget(const EdgeTarget& target) {
    for (const auto& instance : instances_) {
        auto targets = discovery_.enumerateTargets(instance);
        for (const auto& t : targets) {
            if (t.id == target.id) {
                return instance;
            }
        }
    }
    return std::nullopt;
}

} // namespace edgemon
