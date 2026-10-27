#pragma once

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace codesearch {

// Thrown when a program can't be started, or its pipes fail or end early.
class ProcessError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;  // reuse runtime_error's constructors
};

// A running program that we talk to through pipes.
//
// The program is started directly with an argument list, never through a
// shell, so arguments are passed as they are and can't run extra commands.
// Its error output is read on a separate thread the whole time, so it can
// never get stuck waiting for us to read it.
class Process {
public:
    // `program` is found through PATH. `args` don't include the program name.
    Process(const std::string& program, const std::vector<std::string>& args);
    ~Process();

    // One process, one owner: no copying or moving.
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    Process(Process&&) = delete;
    Process& operator=(Process&&) = delete;

    // Sends bytes to the program's input. Throws if the program has exited.
    void write(std::string_view data);
    // Tells the program there is no more input.
    void closeInput();

    // Reads one line of output without the '\n'. Returns false at the end of
    // the output. Throws if the output ends in the middle of a line.
    bool readLine(std::string& line);
    // Reads exactly `n` bytes of output. Throws if the output ends first.
    void readExact(std::size_t n, std::string& out);
    // Reads and throws away exactly `n` bytes of output.
    void skip(std::uint64_t n);
    // Reads everything until the end of the output.
    std::string readAll();

    // Closes the pipes, waits for the program to exit and returns its exit
    // code (128 + the signal number if a signal killed it). Output not read
    // yet is lost.
    int wait();
    // Everything the program wrote to its error output. Only complete after wait().
    const std::string& errorOutput() const { return stderr_; }

private:
    // Reads more output into buffer_. Returns false at the end of the output.
    bool fill();

    std::string program_;  // for error messages
    pid_t pid_ = -1;
    int stdinFd_ = -1;
    int stdoutFd_ = -1;
    bool waited_ = false;
    int exitCode_ = -1;

    std::thread stderrReader_;
    std::string stderr_;

    std::string buffer_;         // output read but not handed out yet...
    std::size_t bufferPos_ = 0;  // ...starts here
};

struct ProcessResult {
    int exitCode = -1;
    std::string out;  // what the program printed
    std::string err;  // its error messages
};

// Runs a program with no input, waits for it and returns everything it printed.
ProcessResult runProcess(const std::string& program, const std::vector<std::string>& args);

}  // namespace codesearch
