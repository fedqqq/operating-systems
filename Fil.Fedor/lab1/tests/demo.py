#!/usr/bin/env python3
"""Print a reproducible live demonstration inside the disposable test container."""
import contextlib
import ctypes
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile

import test_lab1 as helpers


def main():
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.prctl(36, 1, 0, 0, 0) != 0:
        raise OSError(ctypes.get_errno(), "prctl")
    print("Live Linux demonstration: variant 20 (15 points)")
    print("UTC:", datetime.now(timezone.utc).isoformat())
    with helpers.isolated_syslog(), tempfile.TemporaryDirectory(prefix="ff-demo-") as temporary:
        root = Path(temporary)
        watched, second = root / "watched", root / "second"
        watched.mkdir()
        second.mkdir()
        config, pidfile = root / "lab1.conf", root / "lab1.pid"
        config.write_text(f"directory={watched}\n")
        pids = []

        def start():
            command = [str(helpers.BIN), str(config), "--pid-file", str(pidfile)]
            print("$", " ".join(command))
            print(subprocess.check_output(command, text=True).strip())
            pid = int(pidfile.read_text())
            pids.append(pid)
            return pid

        def event(name, path):
            def find():
                for line in helpers.text(helpers.EVENT_LOG).splitlines():
                    match = helpers.EVENT.search(line)
                    if match and name in match[1].split(",") and json.loads(match[3]) == str(path):
                        return line
                return None
            return helpers.eventually(find, f"{name}: {path}")

        def reap(pid):
            helpers.eventually(lambda: not helpers.LabTests.alive(pid), "daemon exit")
            waited, status = os.waitpid(pid, 0)
            assert waited == pid and os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0, status

        try:
            pid = start()
            print("$ ps -o pid,ppid,sid,tty,stat,cmd -p", pid)
            print(subprocess.check_output(["ps", "-o", "pid,ppid,sid,tty,stat,cmd", "-p", str(pid)], text=True).strip())
            print("cwd:", os.readlink(f"/proc/{pid}/cwd"))
            print("stdin/stdout/stderr:", ", ".join(os.readlink(f"/proc/{pid}/fd/{fd}") for fd in range(3)))
            print("\nCreate, write, read, chmod, rename and delete a real file:")
            file = watched / "example.txt"
            renamed = watched / "renamed.txt"
            file.write_text("Hello from Linux\n")
            event("CLOSE_WRITE", file)
            assert file.read_text() == "Hello from Linux\n"
            event("ACCESS", file)
            file.chmod(0o640)
            event("ATTRIB", file)
            file.rename(renamed)
            event("MOVED_TO", renamed)
            renamed.unlink()
            event("DELETE", renamed)
            for name, path in (("CREATE", file), ("MODIFY", file), ("ACCESS", file),
                               ("ATTRIB", file), ("MOVED_FROM", file),
                               ("MOVED_TO", renamed), ("DELETE", renamed)):
                print(event(name, path))

            print("\nReload config with SIGHUP after daemon changed cwd to /:")
            config.write_text(f"directory={second}\n")
            os.kill(pid, signal.SIGHUP)
            helpers.eventually(lambda: "Configuration reloaded" in helpers.text(helpers.SERVICE_LOG), "reload")
            reloaded = second / "after-reload.txt"
            reloaded.write_text("new directory\n")
            print(event("CREATE", reloaded))

            print("\nStart again: old daemon must exit, new PID must own the lock:")
            new = start()
            assert new != pid
            reap(pid)
            print(f"old pid={pid}: exited with status 0; new pid={new}: running")
            print("\nStop with SIGTERM:")
            os.kill(new, signal.SIGTERM)
            reap(new)
            assert pidfile.read_text() == ""
            print(f"pid={new}: exited with status 0; PID file is empty")
            helpers.eventually(lambda: helpers.text(helpers.SERVICE_LOG).count("daemon exiting") >= 2,
                               "both exit messages in syslog")
            print("\nActual service log (separate from LOG_LOCAL0 events):")
            for line in helpers.text(helpers.SERVICE_LOG).splitlines():
                if "fil-fedor-lab1" in line:
                    print(line)
            print("\nPASS: all demonstration assertions succeeded.")
        finally:
            for pid in pids:
                if helpers.LabTests.alive(pid):
                    os.kill(pid, signal.SIGTERM)
                    reap(pid)
                with contextlib.suppress(ChildProcessError):
                    os.waitpid(pid, os.WNOHANG)


if __name__ == "__main__":
    main()
