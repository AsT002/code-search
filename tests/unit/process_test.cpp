#include "codesearch/util/process.hpp"

#include <gtest/gtest.h>

#include <csignal>
#include <string>

namespace codesearch {
namespace {

ProcessResult runShell(const std::string& command) {
    return runProcess("sh", {"-c", command});
}

TEST(Process, ReturnsOutputErrorsAndExitCode) {
    const ProcessResult result = runShell("echo out; echo err >&2; exit 3");
    EXPECT_EQ(result.exitCode, 3);
    EXPECT_EQ(result.out, "out\n");
    EXPECT_EQ(result.err, "err\n");
}

TEST(Process, ReportsDeathBySignal) {
    EXPECT_EQ(runShell("kill -9 $$").exitCode, 128 + 9);
}

TEST(Process, FailsToStartAMissingProgram) {
    EXPECT_THROW(runProcess("code-search-no-such-program", {}), ProcessError);
}

TEST(Process, PassesArgumentsWithoutAShell) {
    // A shell would split this into words and run the part after ";".
    const ProcessResult result = runProcess("printf", {"%s", "a b; echo pwned"});
    EXPECT_EQ(result.out, "a b; echo pwned");
}

TEST(Process, LotsOfOutputOnBothPipesDoesNotHang) {
    // Each one is far bigger than a pipe holds. If we only read stdout, the
    // child would block forever writing stderr.
    const ProcessResult result =
        runShell("head -c 1000000 /dev/zero >&2; head -c 1000000 /dev/zero");
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_EQ(result.out.size(), 1000000U);
    EXPECT_EQ(result.err.size(), 1000000U);
}

TEST(Process, TalksBothWays) {
    Process cat("cat", {});
    cat.write("one\ntwo\n");
    std::string line;
    ASSERT_TRUE(cat.readLine(line));
    EXPECT_EQ(line, "one");
    ASSERT_TRUE(cat.readLine(line));
    EXPECT_EQ(line, "two");
    cat.closeInput();
    EXPECT_FALSE(cat.readLine(line));
    EXPECT_EQ(cat.wait(), 0);
}

TEST(Process, ThrowsWhenOutputEndsEarly) {
    Process exact("printf", {"abc"});
    std::string out;
    EXPECT_THROW(exact.readExact(10, out), ProcessError);

    Process line("printf", {"no newline"});
    EXPECT_THROW(line.readLine(out), ProcessError);

    Process skipped("printf", {"abc"});
    EXPECT_THROW(skipped.skip(10), ProcessError);
}

TEST(Process, WritingToAnExitedProgramThrowsInsteadOfKillingUs) {
    // SIGPIPE is at its default here, which would kill the test program if
    // Process didn't block it.
    Process done("true", {});
    done.readAll();  // the output ends when the program exits
    const std::string big(1000000, 'x');
    EXPECT_THROW(done.write(big), ProcessError);
    EXPECT_EQ(done.wait(), 0);
}

TEST(Process, ChildGetsDefaultSigpipeEvenIfWeIgnoreIt) {
    // The shell's `trap` lists signals that aren't at their default.
    static_cast<void>(std::signal(SIGPIPE, SIG_IGN));
    const ProcessResult result = runShell("trap");
    static_cast<void>(std::signal(SIGPIPE, SIG_DFL));
    EXPECT_EQ(result.out.find("PIPE"), std::string::npos) << result.out;
}

}  // namespace
}  // namespace codesearch
