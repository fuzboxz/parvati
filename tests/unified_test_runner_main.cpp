/*
 * Main entry point for the unified test runner
 *
 *   ./parvati_unified_tests              # Run all tests (fork-isolated)
 *   ./parvati_unified_tests list         # List available tests
 *   ./parvati_unified_tests <test-name>  # Run specific test (fork-isolated)
 *   ./parvati_unified_tests a b c        # Run several tests by name
 *
 * FORK-PER-TEST ISOLATION: the suite packs 100+ GUI/DSP harnesses into one
 * binary. Each test constructs processors/editors/threads that outlive the
 * test's own scope (JUCE statics, message-thread singletons, leaked
 * AsyncUpdaters). Running them all in one process made RSS grow monotonically
 * until macOS killed the run mid-suite (exit 137 / SIGKILL), and made the
 * process-exit JUCE leak dump an unattributable sum of every test.
 *
 * Each test therefore runs in a fork()ed child that _exit()s when the test
 * returns: per-test memory is fully reclaimed between tests, a leak/crash/
 * OOM in one test cannot take down the rest of the suite, and the parent only
 * ever holds the (tiny) test registry. _exit() deliberately skips static
 * destructors, so per-child JUCE leak reports do not fire by default; set
 * PARVATI_UNIFIED_INPROCESS=1 to run without forking (single test under a
 * debugger, or to see the exit-time leak report).
 */

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <sys/wait.h>
#include <unistd.h>

#include "unified_test_runner.h"

namespace {

bool inProcessRequested()
{
    const char* e = std::getenv ("PARVATI_UNIFIED_INPROCESS");
    return e != nullptr && e[0] == '1';
}

// Runs one test. Returns 0 on pass, non-zero on failure/kill.
int runTestIsolated (const std::string& name)
{
    if (! inProcessRequested())
    {
        // Flush ALL stdio BEFORE forking: the child inherits a copy of these
        // buffers and would otherwise re-emit the parent's pending output
        // (duplicated "Running:"/"PASS:" lines) when it flushes on _exit().
        std::cout.flush();
        std::cerr.flush();
        std::fflush (nullptr);

        const pid_t pid = fork();
        if (pid == 0)
        {
            // Child: run the test, flush stdio, leave WITHOUT running static
            // destructors (per-test leak noise / aborts are not part of the
            // pass-fail contract).
            const bool ok = unified_test_runner::g_runner.runTest (name);
            std::fflush (nullptr);
            _exit (ok ? 0 : 1);
        }
        if (pid < 0)
        {
            std::perror ("fork");
            return 1;
        }
        int status = 0;
        while (waitpid (pid, &status, 0) < 0 && errno == EINTR) {}
        if (WIFEXITED (status))
            return WEXITSTATUS (status);
        if (WIFSIGNALED (status))
        {
            std::fprintf (stderr, "  (test process killed by signal %d)\n", WTERMSIG (status));
            return 128 + WTERMSIG (status);
        }
        return 1;
    }

    // In-process opt-out (debugging / leak-report inspection).
    return unified_test_runner::g_runner.runTest (name) ? 0 : 1;
}

}  // namespace

int main (int argc, char* argv[])
{
    if (argc > 1 && std::strcmp (argv[1], "list") == 0)
    {
        unified_test_runner::g_runner.listTests();
        return 0;
    }

    const bool all = argc < 2;
    const std::vector<std::string> names = all
        ? unified_test_runner::g_runner.testNames()
        : [&] {
              std::vector<std::string> v;
              for (int i = 1; i < argc; ++i)
                  v.emplace_back (argv[i]);
              return v;
          }();

    int passed = 0;
    int failed = 0;
    for (const auto& name : names)
    {
        std::cout << "\n========================================\n"
                  << "Running: " << name << "\n"
                  << "========================================\n";
        const int rc = runTestIsolated (name);
        if (rc == 0)
        {
            std::cout << "PASS: " << name << "\n";
            ++passed;
        }
        else
        {
            std::cout << "FAIL: " << name << " (exit " << rc << ")\n";
            ++failed;
        }
        std::cout.flush();
    }

    std::cout << "\n========================================\n"
              << "Test Summary\n"
              << "  Passed: " << passed << "\n"
              << "  Failed: " << failed << "\n"
              << "  Total:  " << (passed + failed) << "\n"
              << "========================================\n";
    return failed;
}
