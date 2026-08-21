/*
 * Example tests demonstrating the unified test runner pattern
 */

#include <cmath>
#include "unified_test_runner.h"

// Example: Basic test with CHECK macros
TEST(example_basic_test) {
    int a = 5;
    int b = 10;
    
    CHECK(a < b, "a is less than b");
    CHECK(a + b == 15, "a + b equals 15");
    
    return true;
}

// Example: Test that demonstrates failure
TEST(example_failing_test) {
    int x = 5;
    
    CHECK(x == 5, "x equals 5");
    CHECK(x == 6, "x equals 6 - this will fail");
    
    return true;  // Never reached due to CHECK failure
}

// Example: Test with multiple assertions
TEST(example_multiple_assertions) {
    double pi = 3.14159;
    
    CHECK(std::abs(pi - 3.14) < 0.01, "pi is approximately 3.14");
    CHECK(std::abs(pi - 3.14159) < 0.0001, "pi is approximately 3.14159");
    CHECK(pi > 3.0, "pi is greater than 3");
    
    return true;
}

// Example: Test demonstrating setup/teardown pattern
TEST(example_setup_teardown) {
    // Setup
    int resource = 42;
    
    // Test
    CHECK(resource == 42, "resource initialized correctly");
    resource++;
    CHECK(resource == 43, "resource incremented");
    
    // Teardown (implicit - resource goes out of scope)
    return true;
}

// Example: Test with Parvati-style checks
TEST(example_parvati_style) {
    // Simulate some Parvati-style test
    int note = 60;
    int velocity = 100;
    
    CHECK(note >= 0 && note <= 127, "note is in valid MIDI range");
    CHECK(velocity >= 0 && velocity <= 127, "velocity is in valid MIDI range");
    
    return true;
}

// Example: Test demonstrating timing
TEST(example_timing) {
    // Simulate a timing-related test
    double sampleRate = 48000.0;
    double blockSize = 512.0;
    
    double duration = blockSize / sampleRate;
    CHECK(duration > 0.0, "duration is positive");
    CHECK(duration < 0.1, "duration is less than 100ms");
    
    return true;
}

// Example: Isolation test 1
TEST(example_isolation_1) {
    int state = 0;
    state++;
    CHECK(state == 1, "state incremented to 1");
    return true;
}

// Example: Isolation test 2
TEST(example_isolation_2) {
    int state = 0;
    state += 2;
    CHECK(state == 2, "state incremented to 2");
    return true;
}
