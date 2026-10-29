#!/usr/bin/env python3
"""Exercise the real daemon, /proc, inotify and rsyslog without mock event APIs."""
import contextlib
import ctypes
import json
import os
from pathlib import Path
import re
import resource
import signal
import subprocess
import sys
import tempfile
import time
import unittest

LAB = Path(__file__).resolve().parents[1]
BIN = LAB / "lab1"
EVENT_LOG = Path("/var/log/fil-fedor-disk-monitor.log")
SERVICE_LOG = None
EVENT = re.compile(r'events=(\S+) type=(\w+) path=("(?:\\.|[^"\\])*") cookie=(\d+)')


def eventually(predicate, description, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(0.025)
    raise AssertionError("Timed out: " + description)


def text(path):
    try:
        return path.read_text(errors="replace")
    except FileNotFoundError:
        return ""


@contextlib.contextmanager
def isolated_syslog():
    """Use only in a disposable Linux container; never replace a host logger."""
    global SERVICE_LOG
    if os.geteuid() != 0 or Path("/dev/log").exists():
        raise RuntimeError("--isolated-syslog requires root and an unused /dev/log; run in Docker")
    if EVENT_LOG.exists():
        raise RuntimeError("Refusing to reuse an existing event log in isolated mode")
    with tempfile.TemporaryDirectory(prefix="ff-syslog-") as temporary:
        root = Path(temporary)
        SERVICE_LOG = root / "service.log"
        config = root / "rsyslog.conf"
        config.write_text(
            'module(load="imuxsock" SysSock.RateLimit.Interval="0")\n'
            '$MaxMessageSize 64k\n'
            '$RepeatedMsgReduction off\n'
            f'$IncludeConfig {LAB}/rsyslog/30-fil-fedor-disk-monitor.conf\n'
            f'*.* action(type="omfile" file="{SERVICE_LOG}")\n'
        )
        validation = subprocess.run(["rsyslogd", "-N1", "-f", str(config)], capture_output=True, text=True)
        if validation.returncode:
            raise RuntimeError(validation.stderr)
        with (root / "stderr").open("w+") as stderr:
            logger = subprocess.Popen(["rsyslogd", "-n", "-i", str(root / "rsyslog.pid"),
                                       "-f", str(config)], stdout=stderr, stderr=stderr)
            try:
                eventually(lambda: Path("/dev/log").exists() and logger.poll() is None,
                           "rsyslog listening on /dev/log")
                yield
            finally:
                logger.terminate()
                logger.wait(timeout=5)
                if logger.returncode not in (0, -signal.SIGTERM):
                    stderr.seek(0)
                    raise RuntimeError("rsyslog failed: " + stderr.read())


class LabTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="ff-lab1-")
        self.root = Path(self.temporary.name)
        self.watch = self.root / "watch"
        self.other = self.root / "other"
        self.outside = self.root / "outside"
        for directory in (self.watch, self.other, self.outside):
            directory.mkdir()
        self.config = self.root / "lab1.conf"
        self.config.write_text("# test config\ndirectory = watch\n")
        self.pidfile = self.root / "lab1.pid"
        self.pids = set()
        self.event_offset = len(text(EVENT_LOG))
        self.service_offset = len(text(SERVICE_LOG)) if SERVICE_LOG else 0
        self.env = os.environ.copy()
        for option, prefix in (("ASAN_OPTIONS", "asan"), ("UBSAN_OPTIONS", "ubsan")):
            self.env[option] = self.env.get(option, "") + f":log_path={self.root / prefix}"

    def tearDown(self):
        errors = []
        for pid in self.pids:
            try:
                if self.alive(pid):
                    os.kill(pid, signal.SIGTERM)
                self.wait_dead(pid)
            except Exception as error:
                errors.append(str(error))
                with contextlib.suppress(ProcessLookupError):
                    os.kill(pid, signal.SIGKILL)
        for pattern in ("asan.*", "ubsan.*"):
            for report in self.root.glob(pattern):
                if report.stat().st_size:
                    errors.append(report.read_text(errors="replace"))
        self.temporary.cleanup()
        # Reap failed startup children adopted by the test subreaper.
        while True:
            try:
                if os.waitpid(-1, os.WNOHANG)[0] == 0:
                    break
            except ChildProcessError:
                break
        if errors:
            self.fail("\n".join(errors))

    @staticmethod
    def alive(pid):
        try:
            return Path(f"/proc/{pid}/stat").read_text().split(")", 1)[1].split()[0] != "Z"
        except FileNotFoundError:
            return False

    def wait_dead(self, pid, expected_signal=None):
        eventually(lambda: not self.alive(pid), f"pid {pid} exiting")
        try:
            _, status = os.waitpid(pid, 0)
        except ChildProcessError:
            return
        if expected_signal is None:
            self.assertTrue(os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0,
                            f"Unexpected daemon exit status {status}")
        else:
            self.assertTrue(os.WIFSIGNALED(status) and os.WTERMSIG(status) == expected_signal)

    def launch(self, *args, ok=True, default=False, **kwargs):
        command = [str(BIN)] + ([] if default else [str(self.config)])
        command += ["--pid-file", str(self.pidfile), *args]
        result = subprocess.run(command, cwd=self.root, env=self.env, capture_output=True,
                                text=True, timeout=15, **kwargs)
        if ok:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            match = re.search(r"Started daemon pid=(\d+)", result.stdout)
            self.assertIsNotNone(match, result.stdout)
            pid = int(match[1])
            self.pids.add(pid)
            self.assertEqual(int(self.pidfile.read_text()), pid)
            self.assertTrue(self.alive(pid))
            return pid
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        return result

    def events(self):
        parsed = []
        for match in EVENT.finditer(text(EVENT_LOG)[self.event_offset:]):
            parsed.append((set(match[1].split(",")), match[2], json.loads(match[3]), int(match[4])))
        return parsed

    def event(self, name, path, count=1):
        return eventually(lambda: [row for row in self.events()
                                   if name in row[0] and row[2] == str(path)]
                          if sum(name in row[0] and row[2] == str(path) for row in self.events()) >= count else [],
                          f"{name} {path}")

    def service(self, phrase):
        if SERVICE_LOG is None:
            self.skipTest("Service log assertions require --isolated-syslog")
        return eventually(lambda: phrase in text(SERVICE_LOG)[self.service_offset:], phrase)

    def test_01_cli_errors(self):
        result = subprocess.run([str(BIN), "--help"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0)
        self.assertIn("Usage:", result.stdout)
        for args in (("--unknown",), ("--pid-file",), (str(self.config), "extra")):
            with self.subTest(args=args):
                result = subprocess.run([str(BIN), *args], cwd=self.root, capture_output=True)
                self.assertEqual(result.returncode, 1, result.stderr.decode(errors="replace"))

    def test_02_daemon_context_and_fd_cleanup(self):
        sentinel = self.root / "inherited-file"
        with sentinel.open("w") as inherited:
            pid = self.launch(pass_fds=(inherited.fileno(),))
        proc = Path(f"/proc/{pid}")
        self.assertEqual(os.readlink(proc / "cwd"), "/")
        for fd in range(3):
            self.assertEqual(os.readlink(proc / "fd" / str(fd)), "/dev/null")
        fields = (proc / "stat").read_text().split(")", 1)[1].split()
        self.assertNotEqual(int(fields[3]), pid)  # session leader is the intermediate child
        self.assertEqual(int(fields[4]), 0)       # no controlling terminal
        descriptors = []
        for fd in (proc / "fd").iterdir():
            with contextlib.suppress(FileNotFoundError):
                descriptors.append(os.readlink(fd))
        self.assertNotIn(str(sentinel), descriptors)
        status = (proc / "status").read_text()
        self.assertIn("Umask:\t0077", status)
        mask = int(re.search(r"SigBlk:\s+([0-9a-f]+)", status)[1], 16)
        for sig in (signal.SIGHUP, signal.SIGTERM):
            self.assertTrue(mask & (1 << (sig - 1)))
        self.assertEqual(self.pidfile.stat().st_mode & 0o777, 0o600)

    def test_03_create_write_read_attributes(self):
        self.launch()
        file = self.watch / "example.txt"
        file.write_text("Hello Linux\n")
        self.event("CREATE", file)
        self.event("MODIFY", file)
        self.event("CLOSE_WRITE", file)
        self.assertEqual(file.read_text(), "Hello Linux\n")
        self.event("OPEN", file)
        self.event("ACCESS", file)
        self.event("CLOSE_NOWRITE", file)
        file.chmod(0o640)
        self.event("ATTRIB", file)

    def test_04_rename_cookies_and_delete(self):
        self.launch()
        before, after = self.watch / "before", self.watch / "after"
        before.write_text("keep contents")
        self.event("CLOSE_WRITE", before)
        before.rename(after)
        source = self.event("MOVED_FROM", before)[0]
        target = self.event("MOVED_TO", after)[0]
        self.assertGreater(source[3], 0)
        self.assertEqual(source[3], target[3])
        self.assertEqual(after.read_text(), "keep contents")
        after.unlink()
        self.event("DELETE", after)

    def test_05_existing_recursive_directories(self):
        nested = self.watch / "a" / "b"
        nested.mkdir(parents=True)
        self.launch()
        file = nested / "deep.txt"
        file.write_bytes(b"\x00\xff")
        self.event("CREATE", file)
        self.assertEqual(file.read_bytes(), b"\x00\xff")

    def test_06_new_recursive_directories(self):
        self.launch()
        nested = self.watch / "new" / "deep"
        nested.mkdir(parents=True)
        self.event("OPEN", nested)  # the recursive watch installer scanned this directory
        file = nested / "created.txt"
        file.write_text("watched")
        self.event("CREATE", file)

    def test_07_renamed_directory_paths(self):
        directory = self.watch / "old"
        (directory / "sub").mkdir(parents=True)
        self.launch()
        renamed = self.watch / "renamed"
        directory.rename(renamed)
        self.event("MOVED_TO", renamed)
        file = renamed / "sub" / "after.txt"
        file.write_text("rename")
        self.event("CREATE", file)
        self.assertFalse(any(row[2] == str(directory / "sub" / "after.txt") for row in self.events()))

    def test_08_moved_in_tree_and_moved_out_cleanup(self):
        source = self.outside / "incoming"
        (source / "deep").mkdir(parents=True)
        self.launch()
        inside = self.watch / "incoming"
        source.rename(inside)
        self.event("OPEN", inside / "deep")
        file = inside / "deep" / "inside.txt"
        file.write_text("inside")
        self.event("CREATE", file)
        moved_out = self.outside / "outgoing"
        inside.rename(moved_out)
        self.event("MOVED_FROM", inside)
        time.sleep(0.2)
        (moved_out / "deep" / "outside.txt").write_text("outside")
        time.sleep(0.2)
        self.assertFalse(any(row[2].endswith("/outside.txt") for row in self.events()))

    def test_09_directory_symlinks_not_followed(self):
        (self.watch / "link").symlink_to(self.outside, target_is_directory=True)
        self.launch()
        (self.outside / "not-watched").write_text("outside")
        time.sleep(0.2)
        self.assertFalse(any(row[2].endswith("/not-watched") for row in self.events()))

    def test_10_reload_absolute_config_and_multiple_roots(self):
        pid = self.launch(default=True)
        replacement = self.root / "replacement.conf"
        replacement.write_text("directory=other\ndirectory=outside\n")
        replacement.replace(self.config)
        os.kill(pid, signal.SIGHUP)
        self.service("Configuration reloaded")
        for root in (self.other, self.outside):
            file = root / "after-reload"
            file.write_text("new config")
            self.event("CREATE", file)
        (self.watch / "removed-root").write_text("old config")
        time.sleep(0.2)
        self.assertFalse(any(row[2].endswith("/removed-root") for row in self.events()))

    def test_11_bad_reload_keeps_old_watches(self):
        pid = self.launch()
        for contents in ("unknown=watch\n", "directory=does-not-exist\n", ""):
            with self.subTest(contents=contents):
                self.config.write_text(contents)
                self.service_offset = len(text(SERVICE_LOG)) if SERVICE_LOG else 0
                os.kill(pid, signal.SIGHUP)
                self.service("Configuration reload rejected")
                file = self.watch / f"still-watched-{len(self.events())}"
                file.write_text("previous config")
                self.event("CREATE", file)
        self.config.unlink()
        os.kill(pid, signal.SIGHUP)
        self.assertTrue(self.alive(pid))

    def test_12_invalid_startup_does_not_stop_running_daemon(self):
        pid = self.launch()
        self.config.write_text("directory=missing\n")
        self.launch(ok=False)
        self.assertTrue(self.alive(pid))
        file = self.watch / "still-running"
        file.write_text("old daemon")
        self.event("CREATE", file)

    def test_13_repeat_start_replaces_old_process(self):
        old = self.launch()
        new = self.launch()
        self.assertNotEqual(old, new)
        self.wait_dead(old)
        self.service("Replacing previous daemon")
        file = self.watch / "single-instance"
        file.write_text("one")
        self.event("CREATE", file)
        time.sleep(0.15)
        self.assertEqual(sum("CREATE" in row[0] and row[2] == str(file) for row in self.events()), 1)

    def test_14_stale_and_malformed_pid_files(self):
        for contents in ("2147483647\n", "not-a-pid\n", ""):
            with self.subTest(contents=contents):
                self.pidfile.write_text(contents)
                pid = self.launch()
                os.kill(pid, signal.SIGTERM)
                self.wait_dead(pid)
                self.assertEqual(self.pidfile.read_text(), "")

    def test_15_foreign_pid_and_symlink_pid_are_rejected(self):
        with subprocess.Popen(["sleep", "30"]) as foreign:
            try:
                self.pidfile.write_text(str(foreign.pid) + "\n")
                self.launch(ok=False)
                self.assertIsNone(foreign.poll())
            finally:
                foreign.terminate()
                foreign.wait(timeout=5)
        self.pidfile.unlink()
        untouched = self.root / "untouched"
        untouched.write_text("do not alter")
        self.pidfile.symlink_to(untouched)
        self.launch(ok=False)
        self.assertEqual(untouched.read_text(), "do not alter")

    def test_16_sigkill_recovery(self):
        old = self.launch()
        os.kill(old, signal.SIGKILL)
        self.wait_dead(old, signal.SIGKILL)
        new = self.launch()
        self.assertNotEqual(new, old)
        file = self.watch / "recovered"
        file.write_text("recovered")
        self.event("CREATE", file)

    def test_17_sigterm_logs_and_clears_pid(self):
        pid = self.launch()
        self.service("Daemon started")
        os.kill(pid, signal.SIGTERM)
        self.wait_dead(pid)
        self.assertEqual(self.pidfile.read_text(), "")
        self.service("daemon exiting")

    def test_18_config_validation_and_duplicate_roots(self):
        for contents in ("", "directory=\n", "directory=missing\n", "wrong=watch\n", "directory=/var/log\n"):
            with self.subTest(contents=contents):
                self.config.write_text(contents)
                self.launch(ok=False)
        nested = self.watch / "nested"
        nested.mkdir()
        self.config.write_bytes(b" # comments\r\n\r\ndirectory = watch\r\ndirectory=watch/nested\r\ndirectory=watch\r\n")
        self.launch()
        file = nested / "deduplicated"
        file.write_text("one watch")
        self.event("CREATE", file)
        time.sleep(0.15)
        self.assertEqual(sum("CREATE" in row[0] and row[2] == str(file) for row in self.events()), 1)

    def test_19_special_filenames_are_escaped(self):
        self.launch()
        file = self.watch / 'привет "quoted" \\ newline\nforged.txt'
        file.write_text("unchanged")
        self.event("CREATE", file)
        self.assertEqual(file.read_text(), "unchanged")
        self.assertIn("\\u000a", text(EVENT_LOG)[self.event_offset:])

    def test_20_burst_500_creations(self):
        self.launch()
        names = {str(self.watch / f"burst-{index:04}") for index in range(500)}
        for name in sorted(names):
            Path(name).write_text("burst")
        eventually(lambda: {row[2] for row in self.events() if "CREATE" in row[0]} >= names,
                   "all 500 CREATE events", timeout=10)
        self.assertNotIn("IN_Q_OVERFLOW", text(SERVICE_LOG)[self.service_offset:] if SERVICE_LOG else "")

    def test_21_syslog_routing_uses_real_production_rule(self):
        self.launch()
        file = self.watch / "routing-marker"
        file.write_text("route to local0 log")
        self.event("CREATE", file)
        self.service("Daemon started")
        self.assertNotIn("events=", text(SERVICE_LOG)[self.service_offset:])
        self.assertNotIn("Daemon started", text(EVENT_LOG)[self.event_offset:])
        import syslog
        syslog.openlog("another-program")
        syslog.syslog(syslog.LOG_LOCAL0 | syslog.LOG_INFO, "foreign-local0-marker")
        syslog.closelog()
        self.service("foreign-local0-marker")
        self.assertNotIn("foreign-local0-marker", text(EVENT_LOG)[self.event_offset:])

    def test_22_deleted_root_can_be_restored_with_reload(self):
        pid = self.launch()
        self.watch.rmdir()
        self.event("DELETE_SELF", self.watch)
        self.service("Watch removed")
        self.watch.mkdir()
        os.kill(pid, signal.SIGHUP)
        self.service("Configuration reloaded")
        file = self.watch / "restored"
        file.write_text("restored")
        self.event("CREATE", file)

    def test_23_rename_over_existing_empty_directory(self):
        source = self.watch / "source"
        (source / "deep").mkdir(parents=True)
        destination = self.watch / "destination"
        destination.mkdir()
        self.launch()
        source.rename(destination)
        self.event("MOVED_TO", destination)
        self.service("Watch removed")
        file = destination / "deep" / "replacement-still-watched"
        file.write_text("replacement inode")
        self.event("CREATE", file)

    def test_24_pid_path_cannot_alias_config_or_output(self):
        original = self.config.read_text()
        alias = self.root / "alias"
        alias.symlink_to(self.root, target_is_directory=True)
        self.pidfile = alias / self.config.name
        self.launch(ok=False)
        self.assertEqual(self.config.read_text(), original)
        self.pidfile = EVENT_LOG
        self.launch(ok=False)

    def test_25_startup_resource_failure_preserves_old_instance(self):
        pid = self.launch()
        def low_fd_limit():
            resource.setrlimit(resource.RLIMIT_NOFILE, (6, 6))
        self.launch(ok=False, preexec_fn=low_fd_limit)
        self.assertTrue(self.alive(pid))
        file = self.watch / "survived-fd-error"
        file.write_text("still alive")
        self.event("CREATE", file)

    def test_26_two_concurrent_launches_leave_one_instance(self):
        command = [str(BIN), str(self.config), "--pid-file", str(self.pidfile)]
        processes = [subprocess.Popen(command, cwd=self.root, env=self.env,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                     for _ in range(2)]
        launched = []
        for process in processes:
            stdout, stderr = process.communicate(timeout=15)
            self.assertEqual(process.returncode, 0, stdout + stderr)
            pid = int(re.search(r"Started daemon pid=(\d+)", stdout)[1])
            self.pids.add(pid)
            launched.append(pid)
        final = int(self.pidfile.read_text())
        for pid in launched:
            if pid != final:
                self.wait_dead(pid)
        self.assertTrue(self.alive(final))
        file = self.watch / "concurrent-single"
        file.write_text("single")
        self.event("CREATE", file)
        time.sleep(0.15)
        self.assertEqual(sum("CREATE" in row[0] and row[2] == str(file) for row in self.events()), 1)

    def test_27_real_queue_overflow_is_reported_and_recovered(self):
        limit = int(Path("/proc/sys/fs/inotify/max_queued_events").read_text())
        if limit > 32768:
            self.skipTest("Queue limit too large for a bounded integration test")
        pid = self.launch()
        os.kill(pid, signal.SIGSTOP)
        try:
            eventually(lambda: "State:\tT" in Path(f"/proc/{pid}/status").read_text(), "daemon stopped")
            for index in range(limit // 4 + 128):
                (self.watch / f"overflow-{index:05}").write_text("x")
        finally:
            os.kill(pid, signal.SIGCONT)
        self.service("IN_Q_OVERFLOW: events were lost")
        self.service("Watches rebuilt after overflow")
        file = self.watch / "after-overflow"
        file.write_text("recovered")
        self.event("CREATE", file)

    @unittest.skipUnless(os.geteuid() == 0, "Identity-switch test requires root inside Docker")
    def test_28_daemon_works_as_unprivileged_user(self):
        self.root.chmod(0o755)
        self.config.chmod(0o644)
        for directory in (self.watch, self.other, self.outside):
            directory.chmod(0o755)
        pid_directory = self.root / "user-runtime"
        pid_directory.mkdir(mode=0o777)
        pid_directory.chmod(0o777)
        self.pidfile = pid_directory / "user.pid"
        def unprivileged():
            os.setgroups([])
            os.setgid(65534)
            os.setuid(65534)
        pid = self.launch(preexec_fn=unprivileged)
        self.assertEqual(self.pidfile.stat().st_uid, 65534)
        file = self.watch / "unprivileged-monitor"
        file.write_text("watched without root")
        self.event("CREATE", file)


if __name__ == "__main__":
    # Adopt double-forked children, so tests can assert their real exit statuses
    # and reap them even in containers whose PID 1 does not reap zombies.
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.prctl(36, 1, 0, 0, 0) != 0:  # PR_SET_CHILD_SUBREAPER
        raise OSError(ctypes.get_errno(), "prctl(PR_SET_CHILD_SUBREAPER)")
    isolated = "--isolated-syslog" in sys.argv
    if isolated:
        sys.argv.remove("--isolated-syslog")
    manager = isolated_syslog() if isolated else contextlib.nullcontext()
    with manager:
        suite = unittest.main(verbosity=2, exit=False)
        sys.exit(0 if suite.result.wasSuccessful() else 1)
