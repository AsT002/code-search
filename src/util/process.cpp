#include "codesearch/util/process.hpp"

#include <fcntl.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>

#include <algorithm>
#include <array>
#include <cerrno>
#include <system_error>

// The current process's environment variables, passed on as they are.
// This is a plain C global from the system, so it has no namespace.
extern char**
    environ;  // NOLINT(readability-redundant-declaration,cppcoreguidelines-avoid-non-const-global-variables)

namespace codesearch {

namespace {

std::string errorText(int error) {
    return std::generic_category().message(error);
}

void closeFd(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

// A pipe: bytes written to writeEnd come out of readEnd. Both ends are closed
// when the Pipe goes away, unless we took one by setting it to -1.
struct Pipe {
    int readEnd = -1;
    int writeEnd = -1;

    Pipe() {
        std::array<int, 2> fds{};
        if (::pipe(fds.data()) != 0) {
            throw ProcessError("could not create a pipe: " + errorText(errno));
        }
        readEnd = fds[0];
        writeEnd = fds[1];
        // Close-on-exec: other programs we start later must not get this
        // pipe, or the child would never see the end of its input.
        ::fcntl(readEnd, F_SETFD, FD_CLOEXEC);   // NOLINT(cppcoreguidelines-pro-type-vararg)
        ::fcntl(writeEnd, F_SETFD, FD_CLOEXEC);  // NOLINT(cppcoreguidelines-pro-type-vararg)
    }
    ~Pipe() {
        closeFd(readEnd);
        closeFd(writeEnd);
    }

    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;
    Pipe(Pipe&&) = delete;
    Pipe& operator=(Pipe&&) = delete;
};

// Writing to a pipe whose reader has exited sends us SIGPIPE, which kills the
// whole program by default. While one of these is alive, SIGPIPE is blocked on
// this thread, so the write fails with EPIPE instead. If the write caused a
// SIGPIPE, it's consumed before unblocking, so it never reaches the program.
// This works the same on macOS and Linux and doesn't change how the rest of
// the program handles signals.
class SigpipeGuard {
public:
    SigpipeGuard() {
        sigemptyset(&pipeOnly_);
        sigaddset(&pipeOnly_, SIGPIPE);
        sigset_t pending{};
        sigemptyset(&pending);
        sigpending(&pending);
        // One may already be waiting from someone else; leave that one alone.
        alreadyPending_ = sigismember(&pending, SIGPIPE) == 1;
        pthread_sigmask(SIG_BLOCK, &pipeOnly_, &previous_);
    }

    ~SigpipeGuard() {
        if (!alreadyPending_) {
            sigset_t pending{};
            sigemptyset(&pending);
            sigpending(&pending);
            if (sigismember(&pending, SIGPIPE) == 1) {
                int received = 0;
                sigwait(&pipeOnly_, &received);  // returns at once: it's pending
            }
        }
        pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
    }

    SigpipeGuard(const SigpipeGuard&) = delete;
    SigpipeGuard& operator=(const SigpipeGuard&) = delete;
    SigpipeGuard(SigpipeGuard&&) = delete;
    SigpipeGuard& operator=(SigpipeGuard&&) = delete;

private:
    sigset_t pipeOnly_{};  // a set holding just SIGPIPE
    sigset_t previous_{};  // which signals this thread blocked before
    bool alreadyPending_ = false;
};

// Takes the fd out of `fd`, so whoever held it no longer closes it.
int take(int& fd) {
    const int taken = fd;
    fd = -1;
    return taken;
}

}  // namespace

Process::Process(const std::string& program, const std::vector<std::string>& args)
    : program_(program) {
    std::vector<std::string> argvText = {program};
    argvText.reserve(1 + args.size());
    for (const std::string& arg : args) {
        argvText.push_back(arg);
    }
    // posix_spawn wants a C-style list of char pointers ending in nullptr.
    std::vector<char*> argv;
    argv.reserve(argvText.size() + 1);
    for (std::string& arg : argvText) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    Pipe in;
    Pipe out;
    Pipe err;

    // Tell posix_spawn to connect the pipes to the child's stdin, stdout and
    // stderr.
    posix_spawn_file_actions_t actions{};
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in.readEnd, STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, out.writeEnd, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, err.writeEnd, STDERR_FILENO);
    // The child starts with SIGPIPE handled the normal way and no signals
    // blocked, even if this program changed that for itself.
    posix_spawnattr_t attributes{};
    posix_spawnattr_init(&attributes);
    sigset_t noSignals{};
    sigemptyset(&noSignals);
    sigset_t pipeOnly{};
    sigemptyset(&pipeOnly);
    sigaddset(&pipeOnly, SIGPIPE);
    posix_spawnattr_setsigmask(&attributes, &noSignals);
    posix_spawnattr_setsigdefault(&attributes, &pipeOnly);
    posix_spawnattr_setflags(&attributes,
                             static_cast<short>(POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF));

    // posix_spawnp finds the program through PATH, like a shell would.
    const int result =
        ::posix_spawnp(&pid_, program.c_str(), &actions, &attributes, argv.data(), environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    if (result != 0) {
        throw ProcessError("could not start " + program + ": " + errorText(result));
    }

    // Keep our ends. The child's ends are closed when the Pipes go away below.
    stdinFd_ = take(in.writeEnd);
    stdoutFd_ = take(out.readEnd);
    const int stderrFd = take(err.readEnd);

    // Read the error output on its own thread until the child closes it.
    stderrReader_ = std::thread([this, stderrFd] {
        std::array<char, 4096> chunk{};
        while (true) {
            const ssize_t n = ::read(stderrFd, chunk.data(), chunk.size());
            if (n > 0) {
                stderr_.append(chunk.data(), static_cast<std::size_t>(n));
            } else if (n == 0 || errno != EINTR) {
                break;
            }
        }
        ::close(stderrFd);
    });
}

Process::~Process() {
    wait();
}

// Not const: it changes the child's state, even though no member of ours changes.
void Process::write(std::string_view data) {  // NOLINT(readability-make-member-function-const)
    const SigpipeGuard guard;
    while (!data.empty()) {
        const ssize_t n = ::write(stdinFd_, data.data(), data.size());
        if (n < 0) {
            if (errno == EINTR) {
                continue;  // interrupted by a signal before writing; try again
            }
            throw ProcessError("could not write to " + program_ + ": " + errorText(errno));
        }
        data.remove_prefix(static_cast<std::size_t>(n));
    }
}

void Process::closeInput() {
    closeFd(stdinFd_);
}

bool Process::fill() {
    // Drop what was already handed out, so the buffer doesn't keep growing.
    buffer_.erase(0, bufferPos_);
    bufferPos_ = 0;

    std::array<char, 65536> chunk{};
    while (true) {
        const ssize_t n = ::read(stdoutFd_, chunk.data(), chunk.size());
        if (n > 0) {
            buffer_.append(chunk.data(), static_cast<std::size_t>(n));
            return true;
        }
        if (n == 0) {
            return false;
        }
        if (errno != EINTR) {
            throw ProcessError("could not read from " + program_ + ": " + errorText(errno));
        }
    }
}

bool Process::readLine(std::string& line) {
    while (true) {
        const std::size_t newline = buffer_.find('\n', bufferPos_);
        if (newline != std::string::npos) {
            line.assign(buffer_, bufferPos_, newline - bufferPos_);
            bufferPos_ = newline + 1;
            return true;
        }
        if (!fill()) {
            if (bufferPos_ < buffer_.size()) {
                throw ProcessError(program_ + " output ended in the middle of a line");
            }
            return false;
        }
    }
}

void Process::readExact(std::size_t n, std::string& out) {
    out.clear();
    out.reserve(n);
    while (out.size() < n) {
        if (bufferPos_ == buffer_.size() && !fill()) {
            throw ProcessError(program_ + " output ended early");
        }
        const std::size_t take = std::min(n - out.size(), buffer_.size() - bufferPos_);
        out.append(buffer_, bufferPos_, take);
        bufferPos_ += take;
    }
}

void Process::skip(std::uint64_t n) {
    while (n > 0) {
        if (bufferPos_ == buffer_.size() && !fill()) {
            throw ProcessError(program_ + " output ended early");
        }
        const std::size_t available = buffer_.size() - bufferPos_;
        const std::size_t take = n < available ? static_cast<std::size_t>(n) : available;
        bufferPos_ += take;
        n -= take;
    }
}

std::string Process::readAll() {
    while (fill()) {
    }
    std::string rest = buffer_.substr(bufferPos_);
    buffer_.clear();
    bufferPos_ = 0;
    return rest;
}

int Process::wait() {
    if (waited_) {
        return exitCode_;
    }
    waited_ = true;

    // Closing stdout means the child can't get stuck writing output nobody reads.
    closeFd(stdinFd_);
    closeFd(stdoutFd_);
    if (stderrReader_.joinable()) {
        stderrReader_.join();
    }

    int status = 0;
    while (::waitpid(pid_, &status, 0) < 0) {
        if (errno != EINTR) {
            return exitCode_;  // -1: we lost track of the process
        }
    }
    if (WIFEXITED(status)) {
        exitCode_ = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        exitCode_ = 128 + WTERMSIG(status);  // same number a shell would show
    }
    return exitCode_;
}

ProcessResult runProcess(const std::string& program, const std::vector<std::string>& args) {
    Process process(program, args);
    process.closeInput();
    ProcessResult result;
    result.out = process.readAll();
    result.exitCode = process.wait();
    result.err = process.errorOutput();
    return result;
}

}  // namespace codesearch
