// build_policy_test — deterministic in-suite guard for the single-binary test
// policy. Complements the configure-time orphan guard in CMakeLists.txt: that
// one fires at `cmake` time, this one fires at RUN TIME by re-checking the
// policy invariants against the SOURCE TREE on disk, so even a hand edit that
// skips/bypasses reconfiguration is still caught by a red suite entry.
//
// Invariants (each prints ok/FAIL lines; any FAIL fails the test):
//   [1] NO PER-TEST TARGETS: no add_executable() other than
//       parvati_unified_tests may reference tests/*.cpp sources
//       (tools/*.cpp EXCLUDE_FROM_ALL utility targets are allowed).
//   [2] ORPHAN-FREE: every tests/*.cpp on disk (minus the two-file harness
//       whitelist, mirroring the configure guard) is listed in the
//       parvati_unified_tests source list.
//   [3] SINGLE MAIN: unified_test_runner_main.cpp is the ONLY tests/*.cpp
//       defining a top-level int main().
//   [4] COUNT SANITY: the runner's registered test count equals the number of
//       TEST( registrations on disk (harness whitelist excluded; the 8 gated
//       example demos compile out under the default PARVATI_TEST_EXAMPLES=OFF)
//       and is at least 100.
//   [5] SCAFFOLDER: tools/new_test.sh exists, is executable, and its text
//       never contains the token "add_executable" — it must not be able to
//       create per-test targets.
//
// Pure file I/O (<2s). Run: ./build_unified/parvati_unified_tests build_policy_test

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#if ! defined (_WIN32)
    #include <unistd.h>  // ::access, X_OK (POSIX exec-bit probe)
#endif

#include <algorithm>
#include <filesystem>

#include "unified_test_runner.h"

#ifndef PARVATI_SOURCE_DIR
#define PARVATI_SOURCE_DIR "."
#endif

namespace {

int g_failures = 0;

void check (bool cond, const std::string& msg)
{
    if (cond)
        std::printf ("  ok  : %s\n", msg.c_str());
    else
    {
        std::printf ("  FAIL: %s\n", msg.c_str());
        ++g_failures;
    }
}

std::string readTextFile (const std::filesystem::path& path)
{
    std::ifstream in (path, std::ios::binary);
    if (! in.is_open())
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string trim (const std::string& s)
{
    const auto b = s.find_first_not_of (" \t\r\n");
    if (b == std::string::npos)
        return {};
    const auto e = s.find_last_not_of (" \t\r\n");
    return s.substr (b, e - b + 1);
}

bool startsWith (const std::string& s, const char* prefix)
{
    return s.rfind (prefix, 0) == 0;
}

bool isTestCppToken (const std::string& tok)
{
    return startsWith (tok, "tests/") && tok.size() > 10
        && tok.compare (tok.size() - 4, 4, ".cpp") == 0;
}

// Same two files the configure-time guard whitelists: the runner's own main
// and the gated demo file (compiled out unless PARVATI_TEST_EXAMPLES=ON).
bool isHarnessWhitelisted (const std::string& fname)
{
    return fname == "unified_test_runner_main.cpp" || fname == "unified_test_examples.cpp";
}

bool lineLooksLikeComment (const std::string& trimmed)
{
    return trimmed.empty() || trimmed[0] == '#' || trimmed.rfind ("//", 0) == 0
        || trimmed.rfind ("/*", 0) == 0 || trimmed.rfind ("*", 0) == 0;
}

std::vector<std::string> splitTokens (const std::string& s)
{
    std::vector<std::string> out;
    std::istringstream ss (s);
    std::string tok;
    while (ss >> tok)
        out.push_back (tok);
    return out;
}

}  // namespace

TEST(build_policy_test)
{
    const std::filesystem::path srcDir (PARVATI_SOURCE_DIR);
    const std::filesystem::path testsDir = srcDir / "tests";
    const std::filesystem::path cmakePath = srcDir / "CMakeLists.txt";

    // ---- shared inputs ----------------------------------------------------
    const std::string cmakeText = readTextFile (cmakePath);
    check (! cmakeText.empty(), "CMakeLists.txt is readable via PARVATI_SOURCE_DIR");

    std::vector<std::string> testFiles;  // bare file names, sorted
    for (const auto& entry : std::filesystem::directory_iterator (testsDir))
        if (entry.is_regular_file() && entry.path().extension() == ".cpp")
            testFiles.push_back (entry.path().filename().string());
    std::sort (testFiles.begin(), testFiles.end());
    check (! testFiles.empty(), "tests/ directory enumerates (" + std::to_string (testFiles.size()) + " .cpp files)");

    // ======================================================================
    // [1] NO PER-TEST TARGETS: scan add_executable( occurrences; any argument
    // referencing tests/*.cpp must belong to parvati_unified_tests.
    // Handles multi-line spans; comment lines are skipped inside a span.
    // ======================================================================
    std::printf ("\n[1] no per-test add_executable targets\n");
    {
        std::vector<std::string> violations;
        bool capturing = false;
        bool captureIsUnified = false;
        std::string captureName;
        std::vector<std::string> captureArgs;

        std::istringstream cm (cmakeText);
        std::string line;
        while (std::getline (cm, line))
        {
            const std::string t = trim (line);
            if (capturing)
            {
                if (lineLooksLikeComment (t))
                    continue;  // e.g. "# DSP Tests (8)" — parens in comments
                               // must not terminate the span
                const auto close = t.find (')');
                if (close == std::string::npos)
                {
                    for (auto& tok : splitTokens (t))
                        if (! startsWith (tok, "#"))
                            captureArgs.push_back (tok);
                    continue;
                }
                for (auto& tok : splitTokens (t.substr (0, close)))
                    if (! startsWith (tok, "#"))
                        captureArgs.push_back (tok);
                capturing = false;
                if (! captureIsUnified)
                    for (const auto& arg : captureArgs)
                        if (isTestCppToken (arg))
                        {
                            violations.push_back (captureName + " -> " + arg);
                            break;
                        }
                continue;
            }

            std::string::size_type p = t.find ("add_executable(");
            while (p != std::string::npos)
            {
                const std::string rest = t.substr (p + 15);
                const auto close = rest.find (')');
                if (close != std::string::npos)
                {
                    const auto toks = splitTokens (rest.substr (0, close));
                    if (! toks.empty() && toks[0] != "parvati_unified_tests")
                        for (size_t i = 1; i < toks.size(); ++i)
                            if (isTestCppToken (toks[i]))
                            {
                                violations.push_back (toks[0] + " -> " + toks[i]);
                                break;
                            }
                }
                else
                {
                    // Span continues on following lines.
                    const auto toks = splitTokens (rest);
                    captureName = toks.empty() ? std::string() : toks[0];
                    captureIsUnified = captureName == "parvati_unified_tests";
                    captureArgs.assign (toks.begin() + (toks.empty() ? 0 : 1), toks.end());
                    capturing = true;
                    break;
                }
                p = t.find ("add_executable(", p + 15);
            }
        }

        check (violations.empty(),
               violations.empty()
                   ? "no add_executable outside parvati_unified_tests references tests/*.cpp"
                   : "per-test target(s) found: " + [&] {
                         std::string joined;
                         for (size_t i = 0; i < violations.size() && i < 10; ++i)
                             joined += (i ? ", " : "") + violations[i];
                         return joined;
                     }());
    }

    // ======================================================================
    // [2] ORPHAN-FREE MIRROR: every tests/*.cpp (minus harness whitelist)
    // appears in the parvati_unified_tests source list.
    // ======================================================================
    std::printf ("\n[2] orphan-free mirror of the configure guard\n");
    {
        std::vector<std::string> listed;
        bool inBlock = false;
        bool closed = false;
        std::istringstream cm (cmakeText);
        std::string line;
        while (std::getline (cm, line))
        {
            const std::string t = trim (line);
            if (! inBlock)
            {
                if (startsWith (t, "add_executable(parvati_unified_tests"))
                    inBlock = true;
                continue;
            }
            if (t == ")")
            {
                closed = true;
                break;
            }
            if (lineLooksLikeComment (t))
                continue;
            for (const auto& tok : splitTokens (t))
                if (isTestCppToken (tok))
                    listed.push_back (tok);
        }
        check (closed, "parvati_unified_tests source list block found ("
                           + std::to_string (listed.size()) + " tests/*.cpp entries)");

        std::vector<std::string> orphans;
        for (const auto& f : testFiles)
        {
            if (isHarnessWhitelisted (f))
                continue;
            const std::string entry = "tests/" + f;
            if (std::find (listed.begin(), listed.end(), entry) == listed.end())
                orphans.push_back (f);
        }
        check (orphans.empty(),
               orphans.empty()
                   ? "every tests/*.cpp (minus harness whitelist) is in the unified source list"
                   : "orphan test file(s) never compiled/run: " + [&] {
                         std::string joined;
                         for (size_t i = 0; i < orphans.size() && i < 10; ++i)
                             joined += (i ? ", " : "") + orphans[i];
                         if (orphans.size() > 10)
                             joined += " (+" + std::to_string (orphans.size() - 10) + " more)";
                         return joined;
                     }());
    }

    // ======================================================================
    // [3] SINGLE MAIN: only unified_test_runner_main.cpp defines int main().
    // ======================================================================
    std::printf ("\n[3] single main() in tests/\n");
    {
        std::vector<std::string> violators;
        for (const auto& f : testFiles)
        {
            if (f == "unified_test_runner_main.cpp")
                continue;
            std::istringstream fs (readTextFile (testsDir / f));
            std::string line;
            while (std::getline (fs, line))
            {
                const std::string t = trim (line);
                if (lineLooksLikeComment (t))
                    continue;
                // top-level definition: "int main(" / "int main ("
                if (startsWith (t, "int"))
                {
                    const auto m = t.find ("main");
                    if (m != std::string::npos)
                    {
                        auto i = t.find_first_not_of (" \t", m + 4);
                        if (i != std::string::npos && t[i] == '(' && t.compare (3, 1, " ") == 0)
                        {
                            violators.push_back (f);
                            break;
                        }
                    }
                }
            }
        }
        check (violators.empty(),
               violators.empty()
                   ? "unified_test_runner_main.cpp is the only tests/*.cpp with int main()"
                   : "extra main() definition(s) would collide at link: " + [&] {
                         std::string joined;
                         for (size_t i = 0; i < violators.size() && i < 10; ++i)
                             joined += (i ? ", " : "") + violators[i];
                         return joined;
                     }());
    }

    // ======================================================================
    // [4] COUNT SANITY: runner registrations == disk TEST( registrations
    // (harness whitelist excluded; examples compile out by default), >= 100.
    // ======================================================================
    std::printf ("\n[4] test-count sanity\n");
    {
        int diskCount = 0;
        for (const auto& f : testFiles)
        {
            if (isHarnessWhitelisted (f))
                continue;
            std::istringstream fs (readTextFile (testsDir / f));
            std::string line;
            while (std::getline (fs, line))
            {
                const std::string t = trim (line);
                if (lineLooksLikeComment (t))
                    continue;
                if (startsWith (t, "TEST("))
                    ++diskCount;
            }
        }
        const int runnerCount = static_cast<int> (
            unified_test_runner::g_runner.testNames().size());

        std::string extra;
        if (runnerCount == diskCount + 8)
            extra = " (delta is exactly 8 — PARVATI_TEST_EXAMPLES appears ON; "
                    "this test pins the default OFF configuration)";
        check (diskCount >= 100, "at least 100 TEST( registrations on disk (got "
                                     + std::to_string (diskCount) + ")");
        check (runnerCount == diskCount,
               "runner registered " + std::to_string (runnerCount)
                   + " tests == " + std::to_string (diskCount) + " on-disk registrations" + extra);
    }

    // ======================================================================
    // [5] SCAFFOLDER INVARIANTS: exists, executable, and can never create
    // per-test targets (its text must not contain the token at all).
    // ======================================================================
    std::printf ("\n[5] tools/new_test.sh invariants\n");
    {
        const std::filesystem::path scaffolder = srcDir / "tools" / "new_test.sh";
        check (std::filesystem::exists (scaffolder), "tools/new_test.sh exists");
#if ! defined (_WIN32)
        check (::access (scaffolder.string().c_str(), X_OK) == 0,
               "tools/new_test.sh is executable (X_OK)");
#else
        // Windows file systems carry no exec bit. The existence check above
        // is the meaningful invariant there.
        std::printf ("  note: exec bit not meaningful on Windows; existence checked only\n");
#endif
        const std::string text = readTextFile (scaffolder);
        check (text.find ("add_executable") == std::string::npos,
               "tools/new_test.sh text contains no 'add_executable' token (cannot create per-test targets)");
    }

    std::printf ("\n%s (%d failures)\n",
                 g_failures ? "BUILD POLICY TEST: FAILURES" : "BUILD POLICY TEST: ALL CHECKS PASSED",
                 g_failures);
    return g_failures == 0;
}
