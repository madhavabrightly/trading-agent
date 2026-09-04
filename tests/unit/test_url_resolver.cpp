#include "edge/UrlUtils.hpp"
#include "core/Types.hpp"
#include <cassert>
#include <iostream>

using namespace edgemon;

void test_normalize() {
    std::cout << "Testing UrlUtils::normalize..." << std::endl;
    // scheme/host lowercasing + trailing slash strip
    assert(UrlUtils::normalize("HTTPS://Example.COM/Path/") == "https://example.com/Path");
    // default port stripped
    assert(UrlUtils::normalize("https://example.com:443/x") == "https://example.com/x");
    assert(UrlUtils::normalize("http://example.com:80/x") == "http://example.com/x");
    // non-default port kept
    assert(UrlUtils::normalize("http://localhost:9222/json") == "http://localhost:9222/json");
    // fragment stripped, query kept
    assert(UrlUtils::normalize("https://example.com/p#frag") == "https://example.com/p");
    assert(UrlUtils::normalize("https://example.com/p?a=1#frag") == "https://example.com/p?a=1");
    // bare host gets https://
    assert(UrlUtils::normalize("example.com") == "https://example.com/");
    std::cout << "  normalize: PASS" << std::endl;
}

void test_matching() {
    std::cout << "Testing UrlUtils::matches..." << std::endl;
    // Exact
    assert(UrlUtils::matches("https://example.com/a",
                             "https://example.com/a", TargetMatchRule::Exact));
    assert(!UrlUtils::matches("https://example.com/a",
                              "https://example.com/b", TargetMatchRule::Exact));
    assert(UrlUtils::matches("example.com/a",
                             "https://example.com/a", TargetMatchRule::Exact));
    // Query difference breaks exact, origin-path tolerates it
    assert(!UrlUtils::matches("https://example.com/a?x=1",
                              "https://example.com/a?x=2", TargetMatchRule::Exact));
    assert(UrlUtils::matches("https://example.com/a?x=1",
                             "https://example.com/a?x=2", TargetMatchRule::OriginPath));
    // URL prefix
    assert(UrlUtils::matches("https://chatgpt.com/c/abc",
                             "https://chatgpt.com/c/abc?x=1", TargetMatchRule::UrlPrefix));
    assert(!UrlUtils::matches("https://chatgpt.com/c/abc",
                              "https://chatgpt.com/c/abcd", TargetMatchRule::Exact));
    // Origin-path ignores query but not path
    assert(!UrlUtils::matches("https://example.com/a",
                              "https://example.com/b", TargetMatchRule::OriginPath));
    // Different hosts never match
    assert(!UrlUtils::matches("https://example.com/a",
                              "https://evil.com/a", TargetMatchRule::UrlPrefix));
    std::cout << "  matching: PASS" << std::endl;
}

void test_same_page() {
    std::cout << "Testing UrlUtils::samePage..." << std::endl;
    assert(UrlUtils::samePage("https://Example.com/x", "https://example.com/x"));
    assert(!UrlUtils::samePage("https://example.com/x", "https://example.com/y"));
    assert(UrlUtils::samePage("https://example.com/x?a=1", "https://example.com/x?a=1"));
    assert(!UrlUtils::samePage("https://example.com/x?a=1", "https://example.com/x?a=2"));
    assert(UrlUtils::samePage("https://example.com/x?a=1", "https://example.com/x?a=2", false));
    std::cout << "  samePage: PASS" << std::endl;
}

int main() {
    test_normalize();
    test_matching();
    test_same_page();
    std::cout << "All URL util tests passed." << std::endl;
    return 0;
}
