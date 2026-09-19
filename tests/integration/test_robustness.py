#!/usr/bin/env python3
"""Integration tests for process robustness (suite id `robust`).

Goal: no single bad file, racing process or occupied port may abort a MiniDrive process.
- Clients started concurrently in one working directory all exit cleanly (they used to race on
  one fixed partmeta.json.tmp and abort with SIGABRT).
- A corrupt client partmeta.json no longer bricks the working directory: the client starts and
  quarantines the file to partmeta.json.corrupt.
- A corrupt server-side partmeta.json no longer terminates the multi-user server when its owner
  logs in; the owner loses only resume state and bystanders are unaffected.
- A corrupt users.json fails CLOSED: startup is refused, a running server refuses account logins
  but stays up, and the file is byte-identical afterwards. The byte-identical check is the
  data-loss guard - a fail-soft Database would rewrite it with a single account on the next
  registration.
- A port that is already in use gives rc=1 and a message naming the port, not an abort.

Every case uses a fresh username: five bad passwords lock an account for 30s (step 9).

Usage:
    python3 tests/integration/test_robustness.py
"""

import os
import subprocess
import sys
import time

from test_utils import (
    TestResult,
    TestEnvironment,
    check_executables,
    CLIENT_EXE,
    SERVER_EXE,
)

SERVER_PORT = 9044
SPARE_PORT = 9045   # Nothing listens here; used where no server must be reachable

TRUNCATED_PARTMETA = '{\n "entries": [\n  {\n   "id": 1,\n   "abso'


class RobustEnv(TestEnvironment):
    def __init__(self):
        super().__init__("robust", SERVER_PORT)

    def server_alive(self):
        return self.server_process is not None and self.server_process.poll() is None

    def login(self, username, password, commands, label, timeout=30):
        """Log in as an existing account and run commands. Returns (stdout, rc)."""
        workdir = self.new_workdir(label)
        proc = self.start_client_process(f"{username}@127.0.0.1:{self.port}",
                                         os.path.join(self.log_dir, f"{label}_client.log"), cwd=workdir)
        data = "\n".join([password] + commands + ["EXIT"]) + "\n"
        try:
            out, _ = proc.communicate(input=data, timeout=timeout)
            rc = proc.returncode
        except subprocess.TimeoutExpired:
            proc.kill()
            out, _ = proc.communicate()
            rc = -1
        with open(os.path.join(self.log_dir, f"{label}_stdout.log"), "w") as f:
            f.write(f"# Exit code: {rc}\n{out}")
        return out, rc

    def user_partmeta(self, username):
        return os.path.join(self.server_root, "private", username, ".partial", "partmeta.json")

    def users_json(self):
        return os.path.join(self.server_root, "users.json")

    def start_expecting_exit(self, port, timeout=10):
        """Start a second server and wait for it to exit. Returns (rc, output)."""
        proc = subprocess.Popen([SERVER_EXE, "--port", str(port), "--root", self.server_root],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            out, _ = proc.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, _ = proc.communicate()
            return None, out
        return proc.returncode, out


def authenticated(stdout):
    return "authentication successful" in stdout.lower() or "registration successful" in stdout.lower()


def test_concurrent_clients_one_workdir(env, results):
    """Bug B: N clients launched at once in one directory must all exit 0."""
    name = "Concurrent clients sharing one working directory"
    workdir = env.new_workdir("ctor_race")
    procs = [subprocess.Popen([CLIENT_EXE, f"127.0.0.1:{SPARE_PORT}"], stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, cwd=workdir)
             for _ in range(8)]
    outputs = []
    for p in procs:
        try:
            out, _ = p.communicate(input="EXIT\n", timeout=15)
        except subprocess.TimeoutExpired:
            p.kill()
            out, _ = p.communicate()
        outputs.append((p.returncode, out))

    bad = [(rc, out) for rc, out in outputs if rc != 0 or "terminate called" in out]
    leftovers = [f for f in os.listdir(os.path.join(workdir, "data", "client_cwd", ".partial"))
                 if f.endswith(".tmp")]
    if bad:
        results.fail(name, f"{len(bad)}/8 clients failed, e.g. rc={bad[0][0]}: {bad[0][1].strip()[:200]}")
    elif leftovers:
        results.fail(name, f"temp files left behind: {leftovers}")
    else:
        results.ok(name)


def test_corrupt_client_partmeta(env, results):
    """A truncated client partmeta.json must not stop the client from starting."""
    name = "Corrupt client partmeta.json is quarantined, client still starts"
    workdir = env.new_workdir("client_corrupt")
    partial = os.path.join(workdir, "data", "client_cwd", ".partial")
    os.makedirs(partial, exist_ok=True)
    with open(os.path.join(partial, "partmeta.json"), "w") as f:
        f.write(TRUNCATED_PARTMETA)

    rcs = []
    for _ in range(2):  # the second run proves the directory is not bricked
        p = subprocess.run([CLIENT_EXE, f"127.0.0.1:{SPARE_PORT}"], input="EXIT\n", capture_output=True,
                           text=True, cwd=workdir, timeout=15)
        rcs.append((p.returncode, p.stdout + p.stderr))

    corrupt = os.path.join(partial, "partmeta.json.corrupt")
    if any(rc != 0 for rc, _ in rcs):
        results.fail(name, f"client rc={[rc for rc, _ in rcs]}: {rcs[0][1].strip()[:200]}")
    elif not os.path.exists(corrupt):
        results.fail(name, "corrupt file was not moved aside to partmeta.json.corrupt")
    else:
        with open(corrupt) as f:
            if f.read() != TRUNCATED_PARTMETA:
                results.fail(name, "quarantined file does not hold the original bytes")
                return
        results.ok(name)


def test_corrupt_server_partmeta(env, results):
    """One user's truncated partmeta.json must not take the server down for everyone."""
    name = "Corrupt server partmeta.json does not abort the server"
    victim, bystander, pw = "robust_victim", "robust_bystander", "pw_robust_1"
    for user in (victim, bystander):
        out, _ = env.register_user(user, pw, f"register_{user}")
        if not authenticated(out):
            results.fail(name, f"setup: could not register {user}")
            return

    # The per-user PartialMetadata is cached for the server's lifetime, so the realistic order is
    # the one a crash produces: the file is damaged while the server is down, read on next start.
    env.stop_server()
    with open(env.user_partmeta(victim), "w") as f:
        f.write(TRUNCATED_PARTMETA)
    env.start_server()

    out_v, _ = env.login(victim, pw, ["LIST"], "victim_login")
    time.sleep(0.3)
    if not env.server_alive():
        results.fail(name, f"server died after the victim logged in (rc={env.server_process.poll()})")
        env.start_server()
        return
    out_b, _ = env.login(bystander, pw, ["LIST"], "bystander_login")
    if not authenticated(out_b):
        results.fail(name, "bystander could not authenticate afterwards")
    elif not authenticated(out_v):
        results.fail(name, "victim could not log in - losing resume state should be the only cost")
    elif not os.path.exists(env.user_partmeta(victim) + ".corrupt"):
        results.fail(name, "server did not quarantine the corrupt file")
    else:
        results.ok(name)


def test_corrupt_users_json_at_startup(env, results):
    """A corrupt users.json is refused at boot, and left untouched."""
    name = "Corrupt users.json refuses startup and is left byte-identical"
    out, _ = env.register_user("robust_boot", "pw_robust_2", "register_boot")
    if not authenticated(out):
        results.fail(name, "setup: could not register")
        return

    env.stop_server()
    with open(env.users_json(), "rb") as f:
        original = f.read()
    damaged = original[: len(original) // 2]
    with open(env.users_json(), "wb") as f:
        f.write(damaged)

    rc, output = env.start_expecting_exit(SERVER_PORT)
    with open(env.users_json(), "rb") as f:
        after = f.read()
    with open(env.users_json(), "wb") as f:  # restore for the following tests
        f.write(original)
    env.start_server()

    if rc != 1:
        results.fail(name, f"expected rc=1, got {rc}: {output.strip()[-200:]}")
    elif "terminate called" in output:
        results.fail(name, "server aborted instead of exiting cleanly")
    elif after != damaged:
        results.fail(name, "users.json was modified")
    else:
        results.ok(name)


def test_corrupt_users_json_at_runtime(env, results):
    """A running server must refuse logins, stay up, never write the file, and recover once restored."""
    name = "Corrupt users.json at runtime fails closed and recovers"
    user, pw = "robust_runtime", "pw_robust_3"
    out, _ = env.register_user(user, pw, "register_runtime")
    if not authenticated(out):
        results.fail(name, "setup: could not register")
        return

    with open(env.users_json(), "rb") as f:
        original = f.read()
    damaged = original[: len(original) // 2]
    with open(env.users_json(), "wb") as f:
        f.write(damaged)

    out_existing, _ = env.login(user, pw, ["LIST"], "runtime_existing")
    # A brand-new name is the dangerous path: registration would push onto an empty list and save
    out_new, _ = env.login("robust_newcomer", "y\npw_robust_4", ["LIST"], "runtime_newcomer")
    alive = env.server_alive()
    with open(env.users_json(), "rb") as f:
        after = f.read()

    with open(env.users_json(), "wb") as f:
        f.write(original)
    out_restored, _ = env.login(user, pw, ["LIST"], "runtime_restored")

    if not alive:
        results.fail(name, f"server died (rc={env.server_process.poll()})")
        env.start_server()
    elif after != damaged:
        results.fail(name, "users.json was rewritten while unreadable - accounts would have been destroyed")
    elif authenticated(out_existing) or authenticated(out_new):
        results.fail(name, "a login succeeded against an unreadable account database")
    elif "500" not in out_existing or "500" not in out_new:
        results.fail(name, "expected a 500 'Account database is unavailable.' answer")
    elif not authenticated(out_restored):
        results.fail(name, "login did not recover after users.json was restored")
    else:
        results.ok(name)


def test_port_in_use(env, results):
    """Bug A: a second server on a taken port exits 1 with a clear message."""
    name = "Port already in use gives rc=1 and names the port"
    rc, output = env.start_expecting_exit(SERVER_PORT)
    if rc != 1:
        results.fail(name, f"expected rc=1, got {rc}: {output.strip()[-200:]}")
    elif "terminate called" in output:
        results.fail(name, "server aborted instead of exiting cleanly")
    elif str(SERVER_PORT) not in output or "already running" not in output:
        results.fail(name, f"message does not name the port: {output.strip()[-200:]}")
    elif not env.server_alive():
        results.fail(name, "the original server stopped")
    else:
        results.ok(name)


def main():
    check_executables()
    env = RobustEnv()
    results = TestResult(env.log_dir)
    env.setup_server_root()
    env.start_server()
    try:
        print("Robustness tests")
        test_concurrent_clients_one_workdir(env, results)
        test_corrupt_client_partmeta(env, results)
        test_corrupt_server_partmeta(env, results)
        test_corrupt_users_json_at_startup(env, results)
        test_corrupt_users_json_at_runtime(env, results)
        test_port_in_use(env, results)
    finally:
        env.stop_server()
    sys.exit(0 if results.summary() else 1)


if __name__ == "__main__":
    main()
