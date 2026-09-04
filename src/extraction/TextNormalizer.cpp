#include "extraction/TextNormalizer.hpp"
#include "logger.hpp"
#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>

namespace edgemon {

std::string TextNormalizer::normalize(const std::string& text) {
    return applyTransforms(text);
}

std::vector<std::string> TextNormalizer::chunk(const std::string& text, size_t maxChunkSize) {
    std::vector<std::string> chunks;
    
    if (text.empty() || maxChunkSize == 0) {
        return chunks;
    }

    std::istringstream stream(text);
    std::string current;
    std::string line;
    
    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.empty()) continue;
        
        if (current.length() + line.length() + 1 <= maxChunkSize) {
            if (!current.empty()) current += " ";
            current += line;
        } else {
            if (!current.empty()) {
                chunks.push_back(current);
            }
            current = line;
            
            while (current.length() > maxChunkSize) {
                chunks.push_back(current.substr(0, maxChunkSize));
                current = current.substr(maxChunkSize);
            }
        }
    }
    
    if (!current.empty()) {
        chunks.push_back(current);
    }
    
    return chunks;
}

std::vector<std::string> TextNormalizer::deduplicate(const std::vector<std::string>& texts) {
    std::vector<std::string> result;
    std::unordered_set<std::string> seen;
    
    for (const auto& text : texts) {
        std::string normalized = normalize(text);
        if (seen.insert(normalized).second) {
            result.push_back(text);
        }
    }
    
    return result;
}

std::string TextNormalizer::removeUrls(const std::string& text) {
    std::regex urlPattern(R"(https?://\S+)");
    return std::regex_replace(text, urlPattern, "");
}

std::string TextNormalizer::removeEmails(const std::string& text) {
    std::regex emailPattern(R"([a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,})");
    return std::regex_replace(text, emailPattern, "");
}

std::string TextNormalizer::removePhoneNumbers(const std::string& text) {
    std::regex phonePattern(R"(\+?[\d\s\-\(\)]{10,})");
    return std::regex_replace(text, phonePattern, "");
}

std::string TextNormalizer::removeSpecialChars(const std::string& text) {
    std::regex specialPattern(R"([^a-zA-Z0-9\s])");
    return std::regex_replace(text, specialPattern, " ");
}

std::string TextNormalizer::toLower(const std::string& text) {
    std::string result = text;
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

std::string TextNormalizer::trim(const std::string& text) {
    size_t start = 0;
    while (start < text.length() && std::isspace(text[start])) {
        start++;
    }
    
    size_t end = text.length();
    while (end > start && std::isspace(text[end - 1])) {
        end--;
    }
    
    return text.substr(start, end - start);
}

std::string TextNormalizer::collapseWhitespace(const std::string& text) {
    std::string result = text;
    std::regex wsPattern(R"(\s+)");
    result = std::regex_replace(result, wsPattern, " ");
    return trim(result);
}

void TextNormalizer::addStopWord(const std::string& word) {
    stopWords_.insert(toLower(word));
}

void TextNormalizer::removeStopWord(const std::string& word) {
    stopWords_.erase(toLower(word));
}

std::string TextNormalizer::removeStopWords(const std::string& text) {
    std::istringstream stream(text);
    std::ostringstream result;
    std::string word;
    bool first = true;
    
    while (stream >> word) {
        if (stopWords_.count(toLower(word)) == 0) {
            if (!first) result << " ";
            result << word;
            first = false;
        }
    }
    
    return result.str();
}

std::string TextNormalizer::computeHash(const std::string& text) {
    std::string normalized = normalize(text);
    size_t hash = std::hash<std::string>{}(normalized);
    
    std::ostringstream oss;
    oss << std::hex << hash;
    return oss.str();
}

std::string TextNormalizer::applyTransforms(const std::string& text) {
    std::string result = text;
    result = removeUrls(result);
    result = removeEmails(result);
    result = collapseWhitespace(result);
    result = trim(result);
    return result;
}

}
