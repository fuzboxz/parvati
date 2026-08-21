/*
 * Unified Test Runner - consolidates multiple tests into a single binary
 * 
 * Usage:
 *   #include "unified_test_runner.h"
 *   
 *   TEST(my_test_name) {
 *       CHECK(condition, "description");
 *       return true;  // or return false on failure
 *   }
 *   
 *   int main() {
 *       return unified_test_runner::run_all();
 *   }
 */

#ifndef PARVATI_UNIFIED_TEST_RUNNER_H
#define PARVATI_UNIFIED_TEST_RUNNER_H

#include <string>
#include <map>
#include <functional>
#include <iostream>
#include <vector>

namespace unified_test_runner {

// Test runner registry
class TestRunner {
public:
    static TestRunner& instance() {
        static TestRunner runner;
        return runner;
    }
    
    void registerTest(const std::string& name, std::function<bool()> testFn) {
        tests_[name] = testFn;
    }

    // Ordered test-name list (for fork-per-test execution in the runner main).
    std::vector<std::string> testNames() const {
        std::vector<std::string> names;
        names.reserve (tests_.size());
        for (const auto& [name, _] : tests_)
            names.push_back (name);
        return names;
    }
    
    bool runTest(const std::string& name) {
        auto it = tests_.find(name);
        if (it == tests_.end()) {
            std::cerr << "Unknown test: " << name << std::endl;
            return false;
        }
        return it->second();
    }
    
    void listTests() const {
        std::cout << "Available tests (" << tests_.size() << "):\n";
        for (const auto& [name, _] : tests_) {
            std::cout << "  " << name << "\n";
        }
    }
    
    int runAll() const {
        int passed = 0;
        int failed = 0;
        
        for (const auto& [name, testFn] : tests_) {
            std::cout << "\n========================================\n";
            std::cout << "Running: " << name << "\n";
            std::cout << "========================================\n";
            
            if (testFn()) {
                std::cout << "PASS: " << name << "\n";
                passed++;
            } else {
                std::cout << "FAIL: " << name << "\n";
                failed++;
            }
        }
        
        std::cout << "\n========================================\n";
        std::cout << "Test Summary\n";
        std::cout << "  Passed: " << passed << "\n";
        std::cout << "  Failed: " << failed << "\n";
        std::cout << "  Total:  " << (passed + failed) << "\n";
        std::cout << "========================================\n";
        
        return failed;
    }
    
private:
    std::map<std::string, std::function<bool()>> tests_;
};

// Global runner instance
inline TestRunner& g_runner = TestRunner::instance();

// Test registration macro
#define TEST(name) \
    bool name(); \
    static struct TestRegistrar_##name { \
        TestRegistrar_##name() { \
            unified_test_runner::g_runner.registerTest(#name, name); \
        } \
    } registrar_##name; \
    bool name()

// Assertion macro
#define CHECK(cond, msg) \
    do { \
        if (cond) { \
            std::cout << "  ok  : " << msg << "\n"; \
        } else { \
            std::cout << "  FAIL: " << msg << "\n"; \
            return false; \
        } \
    } while(0)

} // namespace unified_test_runner

#endif // PARVATI_UNIFIED_TEST_RUNNER_H
