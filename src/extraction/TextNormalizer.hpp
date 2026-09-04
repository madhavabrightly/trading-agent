#pragma once

#include "common.hpp"
#include <string>
#include <vector>
#include <unordered_set>

namespace edgemon {

class TextNormalizer {
public:
    TextNormalizer() = default;

    std::string normalize(const std::string& text);
    std::vector<std::string> chunk(const std::string& text, size_t maxChunkSize = 512);
    std::vector<std::string> deduplicate(const std::vector<std::string>& texts);

    std::string removeUrls(const std::string& text);
    std::string removeEmails(const std::string& text);
    std::string removePhoneNumbers(const std::string& text);
    std::string removeSpecialChars(const std::string& text);

    std::string toLower(const std::string& text);
    std::string trim(const std::string& text);
    std::string collapseWhitespace(const std::string& text);

    void addStopWord(const std::string& word);
    void removeStopWord(const std::string& word);
    std::string removeStopWords(const std::string& text);

    std::string computeHash(const std::string& text);

private:
    std::string applyTransforms(const std::string& text);
    
    std::unordered_set<std::string> stopWords_ = {
        "the", "a", "an", "and", "or", "but", "in", "on", "at", "to", "for",
        "of", "with", "by", "from", "as", "is", "was", "are", "were", "been",
        "be", "have", "has", "had", "do", "does", "did", "will", "would",
        "could", "should", "may", "might", "must", "shall", "can", "need",
        "it", "its", "this", "that", "these", "those", "i", "you", "he",
        "she", "we", "they", "what", "which", "who", "whom", "whose"
    };
};

}
