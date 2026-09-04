#pragma once

#include "common.hpp"
#include <string>
#include <vector>
#include <regex>

namespace edgemon {

class DOMExtractor {
public:
    DOMExtractor() = default;

    ExtractedText extract(const std::string& html, const std::string& targetId);
    std::vector<ExtractedText> extractAll(const std::string& html, const std::string& targetId);

    std::string extractTextContent(const std::string& html);
    std::vector<std::string> extractLinks(const std::string& html);
    std::vector<std::string> extractImages(const std::string& html);
    std::vector<std::string> extractTables(const std::string& html);

    void setIgnoreSelectors(const std::vector<std::string>& selectors);
    void addIgnoreSelector(const std::string& selector);

private:
    std::string stripHtmlTags(const std::string& html);
    std::string cleanWhitespace(const std::string& text);
    
    std::vector<std::regex> ignorePatterns_;
    
    static constexpr const char* DEFAULT_IGNORE_SELECTORS[] = {
        "script", "style", "noscript", "iframe", "nav", "footer", "header",
        "[role='navigation']", "[role='banner']", "[role='contentinfo']"
    };
};

}
