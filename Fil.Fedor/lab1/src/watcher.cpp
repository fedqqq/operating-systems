#include "watcher.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <sys/inotify.h>
#include <syslog.h>
#include <system_error>

namespace fs = std::filesystem;

namespace {
bool inside(const fs::path& parent, const fs::path& path) {
    auto p = parent.begin();
    auto c = path.begin();
    for (; p != parent.end(); ++p, ++c) {
        if (c == path.end() || *p != *c) return false;
    }
    return true;
}

std::string quoted(const fs::path& path) {
    std::string result = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char byte : path.string()) {
        if (byte == '"' || byte == '\\') {
            result += '\\';
            result += static_cast<char>(byte);
        } else if (byte < 32 || byte == 127) {
            result += "\\u00";
            result += hex[byte >> 4];
            result += hex[byte & 15];
        } else {
            result += static_cast<char>(byte);
        }
    }
    return result + '"';
}

std::string event_names(std::uint32_t mask) {
    const std::pair<std::uint32_t, const char*> names[] = {
        {IN_ACCESS, "ACCESS"}, {IN_MODIFY, "MODIFY"}, {IN_ATTRIB, "ATTRIB"},
        {IN_CLOSE_WRITE, "CLOSE_WRITE"}, {IN_CLOSE_NOWRITE, "CLOSE_NOWRITE"},
        {IN_OPEN, "OPEN"}, {IN_MOVED_FROM, "MOVED_FROM"}, {IN_MOVED_TO, "MOVED_TO"},
        {IN_CREATE, "CREATE"}, {IN_DELETE, "DELETE"},
        {IN_DELETE_SELF, "DELETE_SELF"}, {IN_MOVE_SELF, "MOVE_SELF"},
        {IN_UNMOUNT, "UNMOUNT"}, {IN_IGNORED, "IGNORED"}
    };
    std::string result;
    for (const auto& [bit, name] : names) {
        if (mask & bit) {
            if (!result.empty()) result += ',';
            result += name;
        }
    }
    return result;
}
} // namespace

Watcher::Watcher(const Config& config) : fd_(::inotify_init1(IN_NONBLOCK | IN_CLOEXEC)), roots_(config.directories) {
    if (fd_.get() < 0) throw std::system_error(errno, std::generic_category(), "inotify_init1");
    for (const auto& directory : config.directories) add_tree(directory);
}

void Watcher::add_tree(const fs::path& root) {
    const auto add = [&](const fs::path& directory) {
        const int watch = ::inotify_add_watch(fd_.get(), directory.c_str(),
            IN_ALL_EVENTS | IN_UNMOUNT | IN_ONLYDIR | IN_DONT_FOLLOW | IN_EXCL_UNLINK);
        if (watch < 0) throw std::system_error(errno, std::generic_category(), "inotify_add_watch " + directory.string());
        watches_[watch] = directory;
    };
    add(root);
    // The default iterator does not follow directory symlinks.
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (fs::is_directory(entry.symlink_status())) add(entry.path());
    }
}

void Watcher::remove_tree(const fs::path& root) {
    // Copy: root can refer to an entry which is erased below.
    const fs::path directory = root;
    for (auto it = watches_.begin(); it != watches_.end();) {
        if (inside(directory, it->second)) {
            ::inotify_rm_watch(fd_.get(), it->first);
            it = watches_.erase(it);
        } else ++it;
    }
}

void Watcher::rename_tree(const fs::path& from, const fs::path& to) {
    for (auto& [watch, path] : watches_) {
        (void)watch;
        if (inside(from, path)) {
            const auto suffix = path.lexically_relative(from);
            path = suffix == "." ? to : to / suffix;
        }
    }
}

void Watcher::expire_moves() {
    const auto now = std::chrono::steady_clock::now();
    for (auto it = moves_.begin(); it != moves_.end();) {
        if (now - it->second.time >= std::chrono::milliseconds(100)) {
            remove_tree(it->second.from);
            it = moves_.erase(it);
        } else ++it;
    }
}

bool Watcher::consume() {
    alignas(inotify_event) char buffer[64 * 1024];
    // Bound each turn so a constant stream of events cannot starve signals.
    for (int batch = 0; batch < 16; ++batch) {
        const auto count = ::read(fd_.get(), buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && errno == EAGAIN) break;
        if (count <= 0) throw std::system_error(count < 0 ? errno : EIO, std::generic_category(), "read inotify");
        for (std::size_t offset = 0; offset < static_cast<std::size_t>(count);) {
            const auto* event = reinterpret_cast<const inotify_event*>(buffer + offset);
            offset += sizeof(inotify_event) + event->len;
            if (event->mask & IN_Q_OVERFLOW) {
                ::syslog(LOG_ERR, "IN_Q_OVERFLOW: events were lost; rebuilding watches");
                return false;
            }
            const auto found = watches_.find(event->wd);
            if (found == watches_.end()) continue;
            const fs::path path = event->len ? found->second / event->name : found->second;
            bool leaving = false;
            for (const auto& [cookie, move] : moves_) {
                (void)cookie;
                if (inside(move.from, path)) leaving = true;
            }
            if (!leaving) {
                ::syslog(LOG_LOCAL0 | LOG_INFO, "events=%s type=%s path=%s cookie=%u",
                    event_names(event->mask).c_str(), ((event->mask & IN_ISDIR) || !event->len) ? "dir" : "file",
                    quoted(path).c_str(), event->cookie);
            }
            if (event->mask & IN_IGNORED) {
                watches_.erase(event->wd);
                continue;
            }
            try {
                if ((event->mask & IN_ISDIR) && (event->mask & IN_MOVED_FROM))
                    moves_[event->cookie] = {path, std::chrono::steady_clock::now()};
                if ((event->mask & IN_ISDIR) && (event->mask & IN_MOVED_TO)) {
                    const auto move = moves_.find(event->cookie);
                    if (move != moves_.end()) {
                        rename_tree(move->second.from, path);
                        moves_.erase(move);
                    } else add_tree(path);
                }
                if ((event->mask & IN_ISDIR) && (event->mask & IN_CREATE)) add_tree(path);
                if (event->mask & (IN_DELETE_SELF | IN_UNMOUNT)) {
                    ::syslog(LOG_WARNING, "Watch removed: %s", quoted(path).c_str());
                    // Remove by watch id: rename(old, existing_empty_dir)
                    // can give the deleted and replacement inode the same
                    // path while their queued events are being processed.
                    ::inotify_rm_watch(fd_.get(), event->wd);
                    watches_.erase(event->wd);
                }
                if ((event->mask & IN_MOVE_SELF) && !leaving) {
                    // A root moved outside the configured path has no watched
                    // parent and therefore no MOVED_FROM/MOVED_TO cookie pair.
                    if (std::find(roots_.begin(), roots_.end(), path) != roots_.end()) {
                        ::syslog(LOG_WARNING, "Watched root moved: %s; reload config to restore", quoted(path).c_str());
                        remove_tree(path);
                    }
                }
            } catch (const std::exception& error) {
                ::syslog(LOG_ERR, "Watch update failed: %s", error.what());
            }
        }
    }
    expire_moves();
    return true;
}
