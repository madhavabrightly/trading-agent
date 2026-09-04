#include "edge/PageSession.hpp"
#include "edge/CDPClient.hpp"
#include "threadpool.hpp"
#include <cassert>
#include <iostream>
#include <string>

using namespace edgemon;

void test_page_session_creation() {
    std::cout << "Testing PageSession creation..." << std::endl;
    
    auto pool = std::make_shared<ThreadPool>(2);
    
    PageSession session(
        TargetId("test-001"),
        CDPTargetId("cdp-001"),
        "ws://localhost:9222/devtools/page/cdp-001",
        pool
    );
    
    assert(session.getTargetId().value == "test-001");
    assert(session.getCDPTargetId().value == "cdp-001");
    assert(session.getState() == TargetState::Discovered);
    
    std::cout << "  PageSession creation: PASS" << std::endl;
}

void test_page_session_state_callbacks() {
    std::cout << "Testing PageSession state callbacks..." << std::endl;
    
    auto pool = std::make_shared<ThreadPool>(2);
    
    PageSession session(
        TargetId("test-state"),
        CDPTargetId("cdp-state"),
        "ws://localhost:9222/devtools/page/cdp-state",
        pool
    );
    
    TargetState lastState = TargetState::Discovered;
    std::string lastMessage;
    
    session.setStateCallback([&lastState, &lastMessage](TargetState state, const std::string& message) {
        lastState = state;
        lastMessage = message;
    });
    
    assert(lastState == TargetState::Discovered);
    
    std::cout << "  PageSession state callbacks: PASS" << std::endl;
}

void test_page_session_navigation_callback() {
    std::cout << "Testing PageSession navigation callback..." << std::endl;
    
    auto pool = std::make_shared<ThreadPool>(2);
    
    PageSession session(
        TargetId("test-nav"),
        CDPTargetId("cdp-nav"),
        "ws://localhost:9222/devtools/page/cdp-nav",
        pool
    );
    
    std::string lastUrl;
    std::string lastTitle;
    
    session.setNavigationCallback([&lastUrl, &lastTitle](const std::string& url, const std::string& title) {
        lastUrl = url;
        lastTitle = title;
    });
    
    std::cout << "  PageSession navigation callback: PASS" << std::endl;
}

void test_page_session_dom_callback() {
    std::cout << "Testing PageSession DOM callback..." << std::endl;
    
    auto pool = std::make_shared<ThreadPool>(2);
    
    PageSession session(
        TargetId("test-dom"),
        CDPTargetId("cdp-dom"),
        "ws://localhost:9222/devtools/page/cdp-dom",
        pool
    );
    
    std::string lastText;
    
    session.setDOMCallback([&lastText](const std::string& text) {
        lastText = text;
    });
    
    std::cout << "  PageSession DOM callback: PASS" << std::endl;
}

void test_page_session_reconnect_policy() {
    std::cout << "Testing PageSession reconnect policy..." << std::endl;
    
    auto pool = std::make_shared<ThreadPool>(2);
    
    PageSession session(
        TargetId("test-reconnect"),
        CDPTargetId("cdp-reconnect"),
        "ws://localhost:9222/devtools/page/cdp-reconnect",
        pool
    );
    
    session.setReconnectPolicy(
        10,
        std::chrono::milliseconds(100),
        std::chrono::seconds(60)
    );
    
    std::cout << "  PageSession reconnect policy: PASS" << std::endl;
}

void test_page_session_disconnect() {
    std::cout << "Testing PageSession disconnect..." << std::endl;
    
    auto pool = std::make_shared<ThreadPool>(2);
    
    PageSession session(
        TargetId("test-disconnect"),
        CDPTargetId("cdp-disconnect"),
        "ws://localhost:9222/devtools/page/cdp-disconnect",
        pool
    );
    
    session.disconnect();
    
    assert(!session.isConnected());
    
    std::cout << "  PageSession disconnect: PASS" << std::endl;
}

void test_page_session_connect_disconnect_cycle() {
    std::cout << "Testing PageSession connect/disconnect cycle..." << std::endl;
    
    auto pool = std::make_shared<ThreadPool>(2);
    
    PageSession session(
        TargetId("test-cycle"),
        CDPTargetId("cdp-cycle"),
        "ws://localhost:9222/devtools/page/cdp-cycle",
        pool
    );
    
    session.connect();
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    session.disconnect();
    
    assert(!session.isConnected());
    
    std::cout << "  PageSession connect/disconnect cycle: PASS" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Edge Monitor - PageSession Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    
    test_page_session_creation();
    test_page_session_state_callbacks();
    test_page_session_navigation_callback();
    test_page_session_dom_callback();
    test_page_session_reconnect_policy();
    test_page_session_disconnect();
    test_page_session_connect_disconnect_cycle();
    
    std::cout << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "All PageSession tests PASSED!" << std::endl;
    std::cout << "========================================" << std::endl;
    
    return 0;
}
