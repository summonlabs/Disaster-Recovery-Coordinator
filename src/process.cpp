#include "drc/process.hpp"

#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <poll.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#endif

namespace drc::process {
namespace {

void append_le32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
}

[[nodiscard]] std::uint32_t read_le32(const std::uint8_t* data) {
    return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8) |
           (static_cast<std::uint32_t>(data[2]) << 16) | (static_cast<std::uint32_t>(data[3]) << 24);
}

// Deadline for a bounded exchange. A budget of zero means "no bound", and the
// caller blocks until the peer answers or closes.
struct ExchangeDeadline {
    bool bounded = false;
    std::chrono::steady_clock::time_point at{};

    explicit ExchangeDeadline(UnixNanos budget_nanos) : bounded(budget_nanos > 0) {
        if (bounded) {
            at = std::chrono::steady_clock::now() + std::chrono::nanoseconds(budget_nanos);
        }
    }

    [[nodiscard]] bool expired() const {
        return bounded && std::chrono::steady_clock::now() >= at;
    }
};

#if defined(_WIN32)
// Windows splits a command line back into arguments with rules that are not
// simple whitespace splitting, so arguments are quoted exactly as the C
// runtime parses them.
[[nodiscard]] std::wstring quote_argument(const std::string& argument) {
    std::wstring wide;
    wide.reserve(argument.size() + 2);
    for (const char c : argument) {
        wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    }
    const bool needs_quotes = argument.empty() || argument.find_first_of(" \t\n\v\"") !=
                                                     std::string::npos;
    if (!needs_quotes) {
        return wide;
    }
    std::wstring out;
    out.push_back(L'"');
    std::size_t backslashes = 0;
    for (const wchar_t c : wide) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
            continue;
        }
        out.append(backslashes, L'\\');
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

[[nodiscard]] Result<std::wstring> to_wide(const std::string& text) {
    if (text.empty()) {
        return std::wstring{};
    }
    std::wstring out;
    out.resize(text.size());
    const int converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                              static_cast<int>(text.size()), out.data(),
                                              static_cast<int>(out.size()));
    if (converted <= 0) {
        return Status{ErrorCode::Invalid, "process argument is not valid UTF-8"};
    }
    out.resize(static_cast<std::size_t>(converted));
    return out;
}

[[nodiscard]] Status close_handle(void*& handle) {
    if (handle == nullptr) {
        return ok_status();
    }
    const HANDLE to_close = static_cast<HANDLE>(handle);
    handle = nullptr;
    if (CloseHandle(to_close) == 0) {
        return Status{ErrorCode::Io, "CloseHandle failed"};
    }
    return ok_status();
}
#else
// The verdict of a reaped child: its exit code, or 128 + signal when it was
// killed. Both platforms report the same shape.
[[nodiscard]] int decode_wait_status(int status) {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return -1;
}

// Reaps a child that has already been killed, so it does not stay a zombie for
// the life of the process. Bounded: SIGKILL cannot be caught, so this returns
// promptly, and the loop exists only so that a process which cannot be
// scheduled at all cannot wedge the caller.
void reap_killed_child(std::uint64_t pid) {
    if (pid == 0) {
        return;
    }
    int status = 0;
    for (int attempt = 0; attempt < 2000; ++attempt) {
        const pid_t done = ::waitpid(static_cast<pid_t>(pid), &status, WNOHANG);
        if (done == static_cast<pid_t>(pid)) {
            return;
        }
        if (done < 0 && errno != EINTR) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
#endif

}  // namespace

Child::Child(Child&& other) noexcept
    : stdin_write_(other.stdin_write_), stdout_read_(other.stdout_read_),
      stderr_read_(other.stderr_read_), process_(other.process_), job_(other.job_),
      pid_(other.pid_), exited_(other.exited_), exit_code_(other.exit_code_) {
    other.stdin_write_ = nullptr;
    other.stdout_read_ = nullptr;
    other.stderr_read_ = nullptr;
    other.process_ = nullptr;
    other.job_ = nullptr;
}

Child& Child::operator=(Child&& other) noexcept {
    if (this != &other) {
        close_handles();
        stdin_write_ = other.stdin_write_;
        stdout_read_ = other.stdout_read_;
        stderr_read_ = other.stderr_read_;
        process_ = other.process_;
        job_ = other.job_;
        pid_ = other.pid_;
        exited_ = other.exited_;
        exit_code_ = other.exit_code_;
        other.stdin_write_ = nullptr;
        other.stdout_read_ = nullptr;
        other.stderr_read_ = nullptr;
        other.process_ = nullptr;
        other.job_ = nullptr;
    }
    return *this;
}

Child::~Child() {
#if defined(_WIN32)
    (void)terminate();
#else
    // A killed child stays a zombie until it is reaped, and nothing guarantees
    // that wait() was called. Windows reaps through the process handle instead.
    const std::uint64_t pid = pid_;
    const bool live = !exited_;
    (void)terminate();
    if (live) {
        reap_killed_child(pid);
    }
#endif
    close_handles();
}

void Child::close_handles() {
#if defined(_WIN32)
    (void)close_handle(stdin_write_);
    (void)close_handle(stdout_read_);
    (void)close_handle(stderr_read_);
    (void)close_handle(process_);
    (void)close_handle(job_);
#else
    if (stdin_write_ != nullptr) {
        (void)::close(static_cast<int>(reinterpret_cast<std::intptr_t>(stdin_write_) - 1));
        stdin_write_ = nullptr;
    }
    if (stdout_read_ != nullptr) {
        (void)::close(static_cast<int>(reinterpret_cast<std::intptr_t>(stdout_read_) - 1));
        stdout_read_ = nullptr;
    }
    if (stderr_read_ != nullptr) {
        (void)::close(static_cast<int>(reinterpret_cast<std::intptr_t>(stderr_read_) - 1));
        stderr_read_ = nullptr;
    }
    process_ = nullptr;
    job_ = nullptr;
#endif
}

#if defined(_WIN32)

Result<std::unique_ptr<Child>> Child::spawn(const SpawnOptions& options) {
    if (options.command.empty()) {
        return Status{ErrorCode::Invalid, "spawn requires at least one argument"};
    }
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE child_stdin_read = nullptr;
    HANDLE parent_stdin_write = nullptr;
    HANDLE parent_stdout_read = nullptr;
    HANDLE child_stdout_write = nullptr;
    HANDLE parent_stderr_read = nullptr;
    HANDLE child_stderr_write = nullptr;

    auto cleanup_pipes = [&]() {
        for (HANDLE* handle : {&child_stdin_read, &parent_stdin_write, &parent_stdout_read,
                               &child_stdout_write, &parent_stderr_read, &child_stderr_write}) {
            if (*handle != nullptr) {
                (void)CloseHandle(*handle);
                *handle = nullptr;
            }
        }
    };

    if (CreatePipe(&child_stdin_read, &parent_stdin_write, &attributes, 0) == 0 ||
        CreatePipe(&parent_stdout_read, &child_stdout_write, &attributes, 0) == 0 ||
        CreatePipe(&parent_stderr_read, &child_stderr_write, &attributes, 0) == 0) {
        cleanup_pipes();
        return Status{ErrorCode::Io, "CreatePipe failed"};
    }
    (void)SetHandleInformation(parent_stdin_write, HANDLE_FLAG_INHERIT, 0);
    (void)SetHandleInformation(parent_stdout_read, HANDLE_FLAG_INHERIT, 0);
    (void)SetHandleInformation(parent_stderr_read, HANDLE_FLAG_INHERIT, 0);

    std::wstring command_line;
    for (const std::string& argument : options.command) {
        Result<std::wstring> quoted = to_wide(argument);
        if (!quoted.ok()) {
            cleanup_pipes();
            return quoted.status();
        }
        if (!command_line.empty()) {
            command_line.push_back(L' ');
        }
        command_line.append(quote_argument(argument));
    }
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');

    std::wstring working_directory;
    if (!options.working_directory.empty()) {
        Result<std::wstring> wide = to_wide(options.working_directory);
        if (!wide.ok()) {
            cleanup_pipes();
            return wide.status();
        }
        working_directory = wide.value();
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = child_stdin_read;
    startup.hStdOutput = child_stdout_write;
    startup.hStdError = child_stderr_write;
    PROCESS_INFORMATION info{};

    const BOOL created = CreateProcessW(
        nullptr, mutable_command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
        working_directory.empty() ? nullptr : working_directory.c_str(), &startup, &info);
    if (created == 0) {
        cleanup_pipes();
        return Status{ErrorCode::Io, "CreateProcessW failed"};
    }
    // Only the child's own ends are closed here. The parent's ends are the
    // transport, and closing them would leave every participant unreachable.
    for (HANDLE* handle : {&child_stdin_read, &child_stdout_write, &child_stderr_write}) {
        if (*handle != nullptr) {
            (void)CloseHandle(*handle);
            *handle = nullptr;
        }
    }

    auto child = std::unique_ptr<Child>(new Child());
    child->stdin_write_ = parent_stdin_write;
    child->stdout_read_ = parent_stdout_read;
    child->stderr_read_ = parent_stderr_read;
    child->process_ = info.hProcess;
    child->pid_ = static_cast<std::uint64_t>(info.dwProcessId);
    (void)CloseHandle(info.hThread);

    // Participants die with the coordinator even when it is killed, so a crash
    // cannot leave orphaned effect endpoints behind.
    const HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                    sizeof(limits)) != 0) {
            if (AssignProcessToJobObject(job, info.hProcess) != 0) {
                child->job_ = job;
            } else {
                (void)CloseHandle(job);
            }
        } else {
            (void)CloseHandle(job);
        }
    }
    return child;
}

Status Child::write(std::span<const std::uint8_t> bytes) {
    if (stdin_write_ == nullptr) {
        return Status{ErrorCode::Invalid, "child input stream is closed"};
    }
    std::size_t written = 0;
    while (written < bytes.size()) {
        DWORD done = 0;
        const DWORD chunk = static_cast<DWORD>(
            (bytes.size() - written) > (1u << 20) ? (1u << 20) : (bytes.size() - written));
        if (WriteFile(static_cast<HANDLE>(stdin_write_), bytes.data() + written, chunk, &done,
                      nullptr) == 0) {
            return Status{ErrorCode::Io, "WriteFile to child failed"};
        }
        if (done == 0) {
            return Status{ErrorCode::Io, "WriteFile to child wrote zero bytes"};
        }
        written += done;
    }
    return ok_status();
}

Status Child::close_stdin() {
    return close_handle(stdin_write_);
}

namespace {

// Reads exactly the requested number of bytes, honouring the deadline.
[[nodiscard]] Result<std::size_t> read_exact_bounded(void* handle,
                                                     std::uint8_t* target,
                                                     std::size_t count,
                                                     const ExchangeDeadline& deadline,
                                                     bool* timed_out) {
    std::size_t filled = 0;
    while (filled < count) {
        DWORD available = 0;
        if (PeekNamedPipe(static_cast<HANDLE>(handle), nullptr, 0, nullptr, &available, nullptr) ==
            0) {
            return Status{ErrorCode::Io, "PeekNamedPipe failed"};
        }
        if (available == 0) {
            if (deadline.expired()) {
                *timed_out = true;
                return filled;
            }
            if (deadline.bounded) {
                Sleep(1);
                continue;
            }
        }
        DWORD done = 0;
        const std::size_t remaining = count - filled;
        const DWORD request = available != 0 && available < remaining
                                  ? available
                                  : static_cast<DWORD>(remaining);
        if (ReadFile(static_cast<HANDLE>(handle), target + filled, request, &done, nullptr) == 0) {
            return Status{ErrorCode::Io, "ReadFile from child failed"};
        }
        if (done == 0) {
            if (deadline.expired()) {
                *timed_out = true;
                return filled;
            }
            Sleep(1);
            continue;
        }
        filled += done;
    }
    return filled;
}

}  // namespace

Result<std::vector<std::uint8_t>> Child::read_frame(std::uint32_t max_frame_bytes,
                                                    UnixNanos budget_nanos) {
    if (stdout_read_ == nullptr) {
        return Status{ErrorCode::Invalid, "child output stream is closed"};
    }
    const ExchangeDeadline deadline{budget_nanos};
    std::uint8_t header[4] = {};
    bool timed_out = false;
    Result<std::size_t> head = read_exact_bounded(stdout_read_, header, sizeof(header), deadline,
                                                  &timed_out);
    if (!head.ok()) {
        return head.status();
    }
    if (timed_out) {
        return Status{ErrorCode::NotReady,
                      "the participant did not begin to answer within the exchange budget"};
    }
    if (head.value() == 0) {
        return std::vector<std::uint8_t>{};  // end of stream
    }
    const std::uint32_t length = read_le32(header);
    if (length > max_frame_bytes) {
        return Status{ErrorCode::Exhausted, "child frame exceeds the accepted maximum"};
    }
    std::vector<std::uint8_t> payload(length);
    if (payload.empty()) {
        return payload;
    }
    Result<std::size_t> body =
        read_exact_bounded(stdout_read_, payload.data(), payload.size(), deadline, &timed_out);
    if (!body.ok()) {
        return body.status();
    }
    if (timed_out) {
        if (body.value() == 0) {
            // Nothing at all was consumed, so the stream is still framed
            // correctly and the request can be tried again.
            return Status{ErrorCode::NotReady,
                          "the participant did not answer within the exchange budget"};
        }
        // A half-read frame means the next read would start inside a payload.
        return Status{ErrorCode::Io,
                      "the participant answered partially within the exchange budget; the reply "
                      "stream is no longer trustworthy"};
    }
    if (body.value() != payload.size()) {
        return Status{ErrorCode::Io, "child closed its output mid-frame"};
    }
    return payload;
}

Status Child::terminate() {
    if (process_ == nullptr || exited_) {
        return ok_status();
    }
    const HANDLE process = static_cast<HANDLE>(process_);
    DWORD code = 0;
    if (GetExitCodeProcess(process, &code) != 0 && code == STILL_ACTIVE) {
        if (TerminateProcess(process, 9) == 0) {
            return Status{ErrorCode::Io, "TerminateProcess failed"};
        }
    }
    return ok_status();
}

Result<int> Child::wait() {
    if (process_ == nullptr) {
        return Status{ErrorCode::Invalid, "child was never started"};
    }
    if (exited_) {
        return exit_code_;
    }
    const HANDLE process = static_cast<HANDLE>(process_);
    if (WaitForSingleObject(process, INFINITE) != WAIT_OBJECT_0) {
        return Status{ErrorCode::Io, "WaitForSingleObject failed"};
    }
    DWORD code = 0;
    if (GetExitCodeProcess(process, &code) == 0) {
        return Status{ErrorCode::Io, "GetExitCodeProcess failed"};
    }
    exited_ = true;
    exit_code_ = static_cast<int>(code);
    return exit_code_;
}

bool Child::running() const {
    if (process_ == nullptr) {
        return false;
    }
    if (exited_) {
        return false;
    }
    DWORD code = 0;
    if (GetExitCodeProcess(static_cast<HANDLE>(process_), &code) == 0) {
        return false;
    }
    return code == STILL_ACTIVE;
}

Result<std::string> Child::read_stderr(std::uint32_t max_bytes) {
    if (stderr_read_ == nullptr) {
        return std::string{};
    }
    DWORD available = 0;
    if (PeekNamedPipe(static_cast<HANDLE>(stderr_read_), nullptr, 0, nullptr, &available, nullptr) ==
        0) {
        return std::string{};
    }
    const DWORD take = available > max_bytes ? max_bytes : available;
    std::string out;
    out.resize(take);
    DWORD done = 0;
    if (take > 0 && ReadFile(static_cast<HANDLE>(stderr_read_), out.data(), take, &done, nullptr) ==
                        0) {
        return std::string{};
    }
    out.resize(done);
    return out;
}

#else  // POSIX

Result<std::unique_ptr<Child>> Child::spawn(const SpawnOptions& options) {
    if (options.command.empty()) {
        return Status{ErrorCode::Invalid, "spawn requires at least one argument"};
    }
    int stdin_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    if (::pipe(stdin_pipe) != 0) {
        return Status{ErrorCode::Io, "pipe failed"};
    }
    if (::pipe(stdout_pipe) != 0) {
        (void)::close(stdin_pipe[0]);
        (void)::close(stdin_pipe[1]);
        return Status{ErrorCode::Io, "pipe failed"};
    }
    if (::pipe(stderr_pipe) != 0) {
        (void)::close(stdin_pipe[0]);
        (void)::close(stdin_pipe[1]);
        (void)::close(stdout_pipe[0]);
        (void)::close(stdout_pipe[1]);
        return Status{ErrorCode::Io, "pipe failed"};
    }

    std::vector<char*> argv;
    argv.reserve(options.command.size() + 1);
    for (const std::string& argument : options.command) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        (void)::close(stdin_pipe[0]);
        (void)::close(stdin_pipe[1]);
        (void)::close(stdout_pipe[0]);
        (void)::close(stdout_pipe[1]);
        (void)::close(stderr_pipe[0]);
        (void)::close(stderr_pipe[1]);
        return Status{ErrorCode::Io, "fork failed"};
    }
    if (pid == 0) {
        (void)::dup2(stdin_pipe[0], STDIN_FILENO);
        (void)::dup2(stdout_pipe[1], STDOUT_FILENO);
        (void)::dup2(stderr_pipe[1], STDERR_FILENO);
        (void)::close(stdin_pipe[0]);
        (void)::close(stdin_pipe[1]);
        (void)::close(stdout_pipe[0]);
        (void)::close(stdout_pipe[1]);
        (void)::close(stderr_pipe[0]);
        (void)::close(stderr_pipe[1]);
        if (!options.working_directory.empty() &&
            ::chdir(options.working_directory.c_str()) != 0) {
            ::_exit(126);
        }
        ::execvp(argv[0], argv.data());
        ::_exit(127);
    }
    (void)::close(stdin_pipe[0]);
    (void)::close(stdout_pipe[1]);
    (void)::close(stderr_pipe[1]);

    auto child = std::unique_ptr<Child>(new Child());
    child->stdin_write_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(stdin_pipe[1]) + 1);
    child->stdout_read_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(stdout_pipe[0]) + 1);
    child->stderr_read_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(stderr_pipe[0]) + 1);
    child->pid_ = static_cast<std::uint64_t>(pid);
    return child;
}

Status Child::write(std::span<const std::uint8_t> bytes) {
    if (stdin_write_ == nullptr) {
        return Status{ErrorCode::Invalid, "child input stream is closed"};
    }
    const int fd = static_cast<int>(reinterpret_cast<std::intptr_t>(stdin_write_) - 1);
    std::size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t done = ::write(fd, bytes.data() + written, bytes.size() - written);
        if (done < 0) {
            if (errno == EINTR) {
                continue;
            }
            return Status{ErrorCode::Io, "write to child failed"};
        }
        written += static_cast<std::size_t>(done);
    }
    return ok_status();
}

Status Child::close_stdin() {
    if (stdin_write_ == nullptr) {
        return ok_status();
    }
    const int fd = static_cast<int>(reinterpret_cast<std::intptr_t>(stdin_write_) - 1);
    stdin_write_ = nullptr;
    if (::close(fd) != 0) {
        return Status{ErrorCode::Io, "close of child input failed"};
    }
    return ok_status();
}

namespace {

[[nodiscard]] Result<std::size_t> read_exact_bounded(int fd,
                                                     std::uint8_t* target,
                                                     std::size_t count,
                                                     const ExchangeDeadline& deadline,
                                                     bool* timed_out) {
    std::size_t filled = 0;
    while (filled < count) {
        if (deadline.bounded) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline.at - std::chrono::steady_clock::now());
            const int wait_ms = remaining.count() <= 0 ? 0 : static_cast<int>(remaining.count());
            struct pollfd descriptor {};
            descriptor.fd = fd;
            descriptor.events = POLLIN;
            const int ready = ::poll(&descriptor, 1, wait_ms);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return Status{ErrorCode::Io, "poll on child failed"};
            }
            if (ready == 0) {
                *timed_out = true;
                return filled;
            }
        }
        const ssize_t done = ::read(fd, target + filled, count - filled);
        if (done < 0) {
            if (errno == EINTR) {
                continue;
            }
            return Status{ErrorCode::Io, "read from child failed"};
        }
        if (done == 0) {
            return filled;  // end of stream
        }
        filled += static_cast<std::size_t>(done);
    }
    return filled;
}

}  // namespace

Result<std::vector<std::uint8_t>> Child::read_frame(std::uint32_t max_frame_bytes,
                                                    UnixNanos budget_nanos) {
    if (stdout_read_ == nullptr) {
        return Status{ErrorCode::Invalid, "child output stream is closed"};
    }
    const ExchangeDeadline deadline{budget_nanos};
    const int fd = static_cast<int>(reinterpret_cast<std::intptr_t>(stdout_read_) - 1);
    std::uint8_t header[4] = {};
    bool timed_out = false;
    Result<std::size_t> head =
        read_exact_bounded(fd, header, sizeof(header), deadline, &timed_out);
    if (!head.ok()) {
        return head.status();
    }
    if (timed_out) {
        return Status{ErrorCode::NotReady,
                      "the participant did not begin to answer within the exchange budget"};
    }
    if (head.value() == 0) {
        return std::vector<std::uint8_t>{};
    }
    const std::uint32_t length = read_le32(header);
    if (length > max_frame_bytes) {
        return Status{ErrorCode::Exhausted, "child frame exceeds the accepted maximum"};
    }
    std::vector<std::uint8_t> payload(length);
    if (payload.empty()) {
        return payload;
    }
    Result<std::size_t> body =
        read_exact_bounded(fd, payload.data(), payload.size(), deadline, &timed_out);
    if (!body.ok()) {
        return body.status();
    }
    if (timed_out) {
        if (body.value() == 0) {
            // Nothing at all was consumed, so the stream is still framed
            // correctly and the request can be tried again.
            return Status{ErrorCode::NotReady,
                          "the participant did not answer within the exchange budget"};
        }
        // A half-read frame means the next read would start inside a payload.
        return Status{ErrorCode::Io,
                      "the participant answered partially within the exchange budget; the reply "
                      "stream is no longer trustworthy"};
    }
    if (body.value() != payload.size()) {
        return Status{ErrorCode::Io, "child closed its output mid-frame"};
    }
    return payload;
}

Status Child::terminate() {
    if (pid_ == 0 || exited_) {
        return ok_status();
    }
    if (::kill(static_cast<pid_t>(pid_), SIGKILL) != 0 && errno != ESRCH) {
        return Status{ErrorCode::Io, "kill failed"};
    }
    return ok_status();
}

Result<int> Child::wait() {
    if (pid_ == 0) {
        return Status{ErrorCode::Invalid, "child was never started"};
    }
    if (exited_) {
        return exit_code_;
    }
    int status = 0;
    for (;;) {
        const pid_t done = ::waitpid(static_cast<pid_t>(pid_), &status, 0);
        if (done < 0) {
            if (errno == EINTR) {
                continue;
            }
            return Status{ErrorCode::Io, "waitpid failed"};
        }
        break;
    }
    exited_ = true;
    exit_code_ = decode_wait_status(status);
    return exit_code_;
}

bool Child::running() const {
    if (pid_ == 0 || exited_) {
        return false;
    }
    // A query, never a reap: waitpid() would consume the exit status and make a
    // later wait() fail with ECHILD, and this method is const because asking
    // whether a child runs must not change anything. kill(pid, 0) only tests
    // for existence, which is what GetExitCodeProcess does on Windows.
    if (::kill(static_cast<pid_t>(pid_), 0) == 0) {
        return true;
    }
    return errno != ESRCH;
}

Result<std::string> Child::read_stderr(std::uint32_t max_bytes) {
    if (stderr_read_ == nullptr) {
        return std::string{};
    }
    const int fd = static_cast<int>(reinterpret_cast<std::intptr_t>(stderr_read_) - 1);
    // Never block: a caller asking for diagnostics must not be left waiting on
    // a stream with nothing to say. This mirrors PeekNamedPipe on Windows.
    for (;;) {
        struct pollfd descriptor {};
        descriptor.fd = fd;
        descriptor.events = POLLIN;
        const int ready = ::poll(&descriptor, 1, 0);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return Status{ErrorCode::Io, "poll on child stderr failed"};
        }
        if (ready == 0) {
            return std::string{};
        }
        break;
    }
    std::string out;
    out.resize(max_bytes);
    const ssize_t done = ::read(fd, out.data(), out.size());
    if (done <= 0) {
        return std::string{};
    }
    out.resize(static_cast<std::size_t>(done));
    return out;
}

#endif

Status Child::write_frame(std::span<const std::uint8_t> payload, std::uint32_t max_frame_bytes) {
    if (payload.size() > max_frame_bytes) {
        return Status{ErrorCode::Exhausted, "payload exceeds the accepted frame size"};
    }
    std::vector<std::uint8_t> frame;
    frame.reserve(payload.size() + 4);
    append_le32(frame, static_cast<std::uint32_t>(payload.size()));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return write(frame);
}

}  // namespace drc::process
