#include "daemon.hpp"
#include "unique_fd.hpp"
#include "watcher.hpp"

#include <cerrno>
#include <charconv>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <poll.h>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <syslog.h>
#include <system_error>
#include <thread>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {
[[noreturn]] void system_error(const std::string& operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

class SignalMask {
public:
    sigset_t set{};
    SignalMask() {
        ::sigemptyset(&set);
        ::sigaddset(&set, SIGHUP);
        ::sigaddset(&set, SIGTERM);
        ::sigaddset(&set, SIGINT);
        if (::sigprocmask(SIG_BLOCK, &set, &old_) < 0) system_error("sigprocmask");
    }
    ~SignalMask() { ::sigprocmask(SIG_SETMASK, &old_, nullptr); }
private:
    sigset_t old_{};
};

class PidFile {
public:
    explicit PidFile(const fs::path& path) : fd_(::open(path.c_str(),
        O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600)) {
        if (fd_.get() < 0) system_error("open PID file");
        struct stat info{};
        if (::fstat(fd_.get(), &info) < 0) system_error("fstat PID file");
        if (!S_ISREG(info.st_mode) || info.st_uid != ::geteuid() || info.st_nlink != 1)
            throw std::runtime_error("PID file must be a regular file owned by this user with one hard link");
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        pid_t notified = -1;
        while (::flock(fd_.get(), LOCK_EX | LOCK_NB) < 0) {
            if (errno != EWOULDBLOCK && errno != EAGAIN) system_error("flock PID file");
            const pid_t old = read_pid();
            if (old > 1 && old != notified && fs::exists("/proc/" + std::to_string(old))) {
                stop_previous(old, info);
                notified = old;
            }
            if (Clock::now() >= deadline) throw std::runtime_error("Timed out waiting for previous daemon");
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        // A live PID without our lock could refer to an unrelated process or
        // to a manually edited file. Never overwrite it or signal it blindly.
        const pid_t old = read_pid();
        if (old > 1 && fs::exists("/proc/" + std::to_string(old)) && old != notified)
            throw std::runtime_error("PID file names a live process which does not hold the daemon lock");
        owned_ = true;
    }

    ~PidFile() {
        // Keep the inode: unlinking a flock'ed file permits a second lock on
        // a newly created file while this process still holds the old one.
        if (owned_ && ::ftruncate(fd_.get(), 0) < 0)
            ::syslog(LOG_ERR, "Cannot clear PID file: %s", std::strerror(errno));
    }

    void write_pid() {
        const auto text = std::to_string(::getpid()) + "\n";
        if (::ftruncate(fd_.get(), 0) < 0) system_error("truncate PID file");
        if (::pwrite(fd_.get(), text.data(), text.size(), 0) != static_cast<ssize_t>(text.size()))
            system_error("write PID file");
        if (::fsync(fd_.get()) < 0) system_error("fsync PID file");
    }

private:
    UniqueFd fd_;
    bool owned_ = false;

    pid_t read_pid() const {
        char text[64];
        const auto count = ::pread(fd_.get(), text, sizeof(text), 0);
        if (count < 0) system_error("read PID file");
        if (count == 0 || count == static_cast<ssize_t>(sizeof(text))) return -1;
        const char* end = text + count;
        if (*(end - 1) == '\n') --end;
        pid_t pid = -1;
        const auto parsed = std::from_chars(text, end, pid);
        return parsed.ec == std::errc{} && parsed.ptr == end ? pid : -1;
    }

    void stop_previous(pid_t pid, const struct stat& info) {
        // Pin the process before inspecting /proc. A recycled numeric PID
        // cannot redirect the final SIGTERM to another process.
        UniqueFd process(static_cast<int>(::syscall(SYS_pidfd_open, pid, 0)));
        if (process.get() < 0) {
            if (errno == ESRCH) return;
            system_error("pidfd_open (requires Linux >= 5.3)");
        }
        const fs::path proc = "/proc/" + std::to_string(pid);
        std::error_code error;
        const auto executable = fs::read_symlink(proc / "exe", error);
        if (error) return; // Exited between checking /proc and opening pidfd.
        if (executable != fs::read_symlink("/proc/self/exe"))
            throw std::runtime_error("Refusing to signal a different executable from PID file");
        bool same_file = false;
        for (const auto& entry : fs::directory_iterator(proc / "fd", error)) {
            struct stat candidate{};
            if (::stat(entry.path().c_str(), &candidate) == 0 &&
                candidate.st_dev == info.st_dev && candidate.st_ino == info.st_ino) same_file = true;
        }
        if (!same_file) throw std::runtime_error("Previous process does not own this PID file");
        ::syslog(LOG_INFO, "Replacing previous daemon pid=%ld", static_cast<long>(pid));
        if (::syscall(SYS_pidfd_send_signal, process.get(), SIGTERM, nullptr, 0) < 0 && errno != ESRCH)
            system_error("pidfd_send_signal");
    }
};

struct Startup {
    pid_t pid = -1;
    char error[256]{};
};

void notify_parent(int fd, const Startup& status) {
    const char* bytes = reinterpret_cast<const char*>(&status);
    std::size_t offset = 0;
    while (offset < sizeof(status)) {
        const auto count = ::write(fd, bytes + offset, sizeof(status) - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        offset += static_cast<std::size_t>(count);
    }
}

void close_descriptors_except(int keep) {
    // Linux close_range avoids a million close() calls with large RLIMIT_NOFILE.
    const bool lower = keep == 3 || ::syscall(SYS_close_range, 3u, static_cast<unsigned>(keep - 1), 0u) == 0;
    const bool upper = ::syscall(SYS_close_range, static_cast<unsigned>(keep + 1), UINT_MAX, 0u) == 0;
    if (lower && upper) return;
    struct rlimit limit{};
    if (::getrlimit(RLIMIT_NOFILE, &limit) < 0) system_error("getrlimit");
    const auto maximum = limit.rlim_max == RLIM_INFINITY ? 1048576 : limit.rlim_max;
    for (rlim_t fd = 3; fd < maximum; ++fd) {
        if (fd != static_cast<rlim_t>(keep)) ::close(static_cast<int>(fd));
    }
}

void event_loop(const fs::path& config_path, Config& config,
                std::unique_ptr<Watcher>& watcher, int signal_fd) {
    bool running = true;
    while (running) {
        pollfd descriptors[] = {{signal_fd, POLLIN, 0}, {watcher->fd(), POLLIN, 0}};
        const int ready = ::poll(descriptors, 2, 50);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) system_error("poll");
        bool reloaded = false;
        if (descriptors[0].revents & POLLIN) {
            signalfd_siginfo signal{};
            while (::read(signal_fd, &signal, sizeof(signal)) == sizeof(signal)) {
                if (signal.ssi_signo == SIGTERM || signal.ssi_signo == SIGINT) {
                    running = false;
                    break;
                }
                if (signal.ssi_signo == SIGHUP) {
                    try {
                        auto replacement = load_config(config_path);
                        auto next = std::make_unique<Watcher>(replacement);
                        watcher.swap(next);
                        config = std::move(replacement);
                        reloaded = true;
                        ::syslog(LOG_INFO, "Configuration reloaded");
                    } catch (const std::exception& error) {
                        ::syslog(LOG_ERR, "Configuration reload rejected; keeping previous watches: %s", error.what());
                    }
                }
            }
        }
        if (!running) break;
        if (!reloaded && (descriptors[1].revents & POLLIN) && !watcher->consume()) {
            // Rebuild on a new descriptor. Report failure and stop if full
            // coverage can no longer be restored after an overflow.
            watcher = std::make_unique<Watcher>(config);
            ::syslog(LOG_INFO, "Watches rebuilt after overflow");
        }
        watcher->expire_moves();
        if ((descriptors[0].revents | descriptors[1].revents) & (POLLERR | POLLHUP | POLLNVAL))
            throw std::runtime_error("Polling descriptor failed");
    }
    ::syslog(LOG_INFO, "SIGTERM/SIGINT received; daemon exiting");
}
} // namespace

Daemon& Daemon::instance() {
    static Daemon daemon;
    return daemon;
}

int Daemon::run(const fs::path& config_path, const fs::path& pid_path, Config config) {
    SignalMask signals; // Block before fork; no asynchronous C++ signal handlers.
    int pipe_fds[2];
    if (::pipe2(pipe_fds, O_CLOEXEC) < 0) system_error("pipe2");
    // Keep the readiness pipe away from stdin/stdout/stderr, even if they
    // were closed by the invoking program.
    for (int& fd : pipe_fds) {
        if (fd < 3) {
            const int duplicate = ::fcntl(fd, F_DUPFD_CLOEXEC, 3);
            if (duplicate < 0) system_error("duplicate readiness pipe");
            ::close(fd);
            fd = duplicate;
        }
    }
    const pid_t first = ::fork();
    if (first < 0) {
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        system_error("fork");
    }
    if (first > 0) {
        ::close(pipe_fds[1]);
        UniqueFd reader(pipe_fds[0]);
        Startup status;
        char* bytes = reinterpret_cast<char*>(&status);
        std::size_t offset = 0;
        while (offset < sizeof(status)) {
            const auto count = ::read(reader.get(), bytes + offset, sizeof(status) - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) break;
            offset += static_cast<std::size_t>(count);
        }
        while (::waitpid(first, nullptr, 0) < 0 && errno == EINTR) {}
        if (offset != sizeof(status)) throw std::runtime_error("Daemon exited before reporting readiness");
        if (status.pid < 0) throw std::runtime_error(status.error);
        std::cout << "Started daemon pid=" << status.pid << '\n';
        return 0;
    }

    ::close(pipe_fds[0]);
    int report = pipe_fds[1];
    try {
        if (::setsid() < 0) system_error("setsid");
        const pid_t second = ::fork();
        if (second < 0) system_error("second fork");
        if (second > 0) ::_exit(0);
        ::closelog();
        close_descriptors_except(report);
        ::umask(0077);
        if (::chdir("/") < 0) system_error("chdir");
        const int null = ::open("/dev/null", O_RDWR);
        if (null < 0) system_error("open /dev/null");
        for (int fd = 0; fd < 3; ++fd) {
            if (::dup2(null, fd) < 0) system_error("dup2 /dev/null");
        }
        if (null > 2) ::close(null);
        ::openlog("fil-fedor-lab1", LOG_PID | LOG_NDELAY, LOG_DAEMON);
        UniqueFd signal_fd(::signalfd(-1, &signals.set, SFD_NONBLOCK | SFD_CLOEXEC));
        if (signal_fd.get() < 0) system_error("signalfd");
        // Construct all watches before replacing an existing instance: a
        // bad configuration or exhausted watch limit must not stop it.
        auto watcher = std::make_unique<Watcher>(config);
        PidFile pid(pid_path);
        pid.write_pid();
        ::syslog(LOG_INFO, "Daemon started pid=%ld roots=%zu", static_cast<long>(::getpid()), config.directories.size());
        Startup status;
        status.pid = ::getpid();
        notify_parent(report, status);
        ::close(report);
        report = -1;
        event_loop(config_path, config, watcher, signal_fd.get());
        return 0;
    } catch (const std::exception& error) {
        ::syslog(LOG_ERR, "Daemon failed: %s", error.what());
        if (report >= 0) {
            Startup status;
            std::snprintf(status.error, sizeof(status.error), "%s", error.what());
            notify_parent(report, status);
            ::close(report);
        }
        return 1;
    }
}
