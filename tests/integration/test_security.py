#!/usr/bin/env python3
"""Integration tests for MiniDrive security properties.

Focus:
- Path traversal must not allow reading outside a user's root.
- A user must not access another user's private files.
- Passwords must not be stored in plain text in the user database.

These are integration tests: they exercise the system via the client CLI.
They do read the test server's on-disk root (test-controlled) to validate
security invariants (e.g., no secret file leakage, no plaintext password).

Usage:
    python3 tests/integration/test_security.py
"""

import os
import re
import sys
import subprocess
import time
import signal
import shutil

from test_utils import (
    TestEnvironment,
    TestResult,
    check_executables,
    SERVER_EXE,
    CLIENT_EXE,
    get_test_log_dir,
    has_error_with_code,
    has_ok_response,
)

SERVER_PORT = 9024


class SecurityTestEnvironment(TestEnvironment):
    def __init__(self, suite_name: str, port: int):
        super().__init__(suite_name, port)

    def _run_client(self, connect_arg: str, input_lines, test_name: str, timeout: int = 30, files_to_create: dict = None):
        self.test_counter += 1
        log_prefix = f"{self.test_counter:02d}_{test_name}"

        stdout_log = os.path.join(self.log_dir, f"{log_prefix}_stdout.log")
        client_log = os.path.join(self.log_dir, f"{log_prefix}_client.log")
        work_dir = os.path.join(self.log_dir, f"{log_prefix}_workdir")
        os.makedirs(work_dir, exist_ok=True)

        if files_to_create:
            for fname, content in files_to_create.items():
                with open(os.path.join(work_dir, fname), "w") as f:
                    f.write(content)

        process = self.start_client_process(connect_arg, client_log, cwd=work_dir)

        try:
            input_data = "\n".join(input_lines) + "\n"
            stdout, _ = process.communicate(input=input_data, timeout=timeout)
            exit_code = process.returncode

            with open(stdout_log, "w") as f:
                f.write(f"# Connect: {connect_arg}\n")
                f.write(f"# Input lines: {input_lines}\n")
                f.write(f"# Exit code: {exit_code}\n")
                f.write(f"# Work dir: {work_dir}\n")
                f.write(f"# {'='*50}\n\n")
                f.write(stdout)

            return stdout, exit_code, work_dir
        except subprocess.TimeoutExpired:
            process.kill()
            with open(stdout_log, "w") as f:
                f.write(f"# TIMEOUT after {timeout}s\n")
            return "TIMEOUT", -1, work_dir

    def run_public(self, commands, test_name: str, timeout: int = 30):
        if isinstance(commands, list):
            input_lines = commands + ["EXIT"]
        else:
            input_lines = [commands, "EXIT"]
        return self._run_client(f"127.0.0.1:{self.port}", input_lines, test_name, timeout)

    def run_user(self, username: str, password: str, commands, test_name: str, timeout: int = 30, files_to_create: dict = None):
        input_lines = [password]
        if isinstance(commands, list):
            input_lines.extend(commands)
        else:
            input_lines.append(commands)
        input_lines.append("EXIT")
        return self._run_client(f"{username}@127.0.0.1:{self.port}", input_lines, test_name, timeout, files_to_create=files_to_create)

    def cleanup(self):
        self.stop_server()
        if os.path.exists(self.server_root):
            shutil.rmtree(self.server_root, ignore_errors=True)


def test_public_path_traversal_download_blocked(env: SecurityTestEnvironment, results: TestResult):
    """Ensure public-mode clients cannot download files outside public root."""
    
    # 1. Upload a file to confirm we are operating in public dir
    probe_name = "probe.txt"
    probe_path = os.path.abspath(probe_name)
    with open(probe_path, "w") as f:
        f.write("Probe content")
    
    # Upload to server
    env.run_public(f"UPLOAD {probe_path} probe.txt", "setup_probe")
    
    if os.path.exists(probe_path):
        os.remove(probe_path)

    # 2. Discover public directory by finding the probe file
    discovered_public_dir = None
    for root, dirs, files in os.walk(env.server_root):
        if "probe.txt" in files:
            discovered_public_dir = root
            break
    
    if not discovered_public_dir:
        results.fail("Security Setup", "Could not locate public directory via probe file")
        return

    print(f"  [Info] Discovered public dir: {discovered_public_dir}")

    # 3. Find a target file outside the public directory
    target_path = None
    for root, dirs, files in os.walk(env.server_root):
        # Skip files inside the discovered public directory
        if os.path.abspath(root).startswith(os.path.abspath(discovered_public_dir)):
            continue

        for f in files:
            full_path = os.path.join(root, f)
            target_path = full_path
            break
        if target_path:
            break
            
    if not target_path:
        target_name = "host_secret.txt"
        target_path = os.path.join(env.server_root, target_name)
        with open(target_path, "w") as f:
            f.write("SECRET: SHOULD NOT LEAK")
    
    # 4. Calculate path from public directory to target
    rel_path = os.path.relpath(target_path, discovered_public_dir)
    print(f"  [Info] Calculated attack path to {os.path.basename(target_path)}: {rel_path}")

    # 4. Try to escape and download
    attempts = [
        (f"DOWNLOAD {rel_path} leaked.txt", "leaked.txt"),
        (f"DOWNLOAD ../{rel_path} leaked2.txt", "leaked2.txt"),
    ]

    for cmd, out_name in attempts:
        stdout, code, work_dir = env.run_public(cmd, f"public_traversal_{out_name}")
        leaked_path = os.path.join(work_dir, out_name)

        if os.path.exists(leaked_path):
            results.fail("Public traversal blocked", f"Leaked file created: {cmd}")
            return
        if "ERROR" in stdout:
            results.ok(f"Public traversal blocked: {cmd}")
        else:
            results.fail("Public traversal blocked", f"Expected 'ERROR' for: {cmd}\nGot: {stdout[:200]}")
            return


def test_user_cannot_access_other_user_private(env: SecurityTestEnvironment, results: TestResult):
    """Ensure an authenticated user cannot read another user's private files."""
    alice = "sec_alice"
    bob = "sec_bob"
    alice_pw = "AlicePlainPassword-123!"
    bob_pw = "BobPlainPassword-456!"

    env.register_user(alice, alice_pw, "register_alice")
    env.register_user(bob, bob_pw, "register_bob")

    # Discover Bob's private directory by uploading a probe file
    probe_name = "bob_probe.txt"
    
    try:
        # Upload as Bob
        env.run_user(bob, bob_pw, f"UPLOAD {probe_name}", "bob_upload_probe", files_to_create={probe_name: "probe"})
        
        # Search for probe file in server root to find Bob's actual storage location
        bob_private_dir = None
        for root, dirs, files in os.walk(env.server_root):
            if probe_name in files:
                bob_private_dir = root
                break
        
        if not bob_private_dir:
            results.fail("Security Setup", "Could not locate Bob's private directory via probe file")
            return
            
        print(f"  [Info] Discovered Bob's private dir: {bob_private_dir}")

        # Create a secret file for Bob in the discovered directory
        bob_secret_name = "bob_secret.txt"
        bob_secret_path = os.path.join(bob_private_dir, bob_secret_name)
        with open(bob_secret_path, "w") as f:
            f.write("BOB SECRET")

        # Now we need to find where Alice is to try to traverse from there
        alice_probe = "alice_probe.txt"
        env.run_user(alice, alice_pw, f"UPLOAD {alice_probe}", "alice_upload_probe", files_to_create={alice_probe: "probe"})
        
        alice_private_dir = None
        for root, dirs, files in os.walk(env.server_root):
            if alice_probe in files:
                alice_private_dir = root
                break
        
        if not alice_private_dir:
             results.fail("Security Setup", "Could not locate Alice's private directory")
             return

        # Calculate relative path from Alice to Bob to attempt traversal
        # This simulates Alice guessing the path to Bob's directory
        rel_path_to_bob = os.path.relpath(bob_private_dir, alice_private_dir)
        print(f"  [Info] Relative path from Alice to Bob: {rel_path_to_bob}")

        attempts = [
            (f"DOWNLOAD {rel_path_to_bob}/{bob_secret_name} leak.txt", "leak.txt"),
            # Also try some standard assumptions if the relative path is simple
            (f"DOWNLOAD ../{os.path.basename(bob_private_dir)}/{bob_secret_name} leak2.txt", "leak2.txt"),
            # Try absolute path from server root (if server allows / to mean root)
            (f"DOWNLOAD /../{os.path.relpath(bob_private_dir, env.server_root)}/{bob_secret_name} leak3.txt", "leak3.txt"),
        ]

        for cmd, out_name in attempts:
            stdout, code, work_dir = env.run_user(alice, alice_pw, cmd, f"alice_traversal_{out_name}")
            leaked_path = os.path.join(work_dir, out_name)

            if os.path.exists(leaked_path):
                results.fail("Cross-user access blocked", f"Leaked file created: {cmd}")
                return
            
            if "ERROR" in stdout:
                results.ok(f"Cross-user access blocked: {cmd}")
            else:
                # If no error, ensure file wasn't actually downloaded (maybe empty response?)
                if not os.path.exists(leaked_path):
                     results.ok(f"Cross-user access blocked (no file): {cmd}")
                else:
                     results.fail("Cross-user access blocked", f"Command succeeded and file leaked: {cmd}")

    finally:
        # Cleanup is handled by env.cleanup() generally, but we can remove probes if we want
        pass


def test_users_db_has_no_plaintext_password(env: SecurityTestEnvironment, results: TestResult):
    """Ensure no file in server root contains the raw password string."""
    username = "plaintext_check"
    password = "PLAIN_TEXT_PASSWORD_SHOULD_NOT_APPEAR"

    env.register_user(username, password, "register_plaintext_check")

    # Force a save to disk by restarting the server.
    env.restart_server()

    username_found = False
    
    for root, dirs, files in os.walk(env.server_root):
        for file in files:
            file_path = os.path.join(root, file)
            try:
                with open(file_path, "r", encoding="utf-8", errors="replace") as f:
                    content = f.read()
                    
                if password in content:
                    results.fail("Password storage", f"Plaintext password found in {file}")
                    return
                
                if username in content:
                    username_found = True
            except Exception:
                # Ignore read errors
                pass

    if not username_found:
        results.fail("Password storage", "Username not found in any server file (DB missing?)")
        return

    results.ok("No plaintext password found in server files")


def error_codes(stdout: str):
    """Every protocol error code in a transcript, as the client actually prints them.

    test_utils.get_error_code() is unusable here: it returns None as soon as "ok" appears anywhere
    earlier (the public-mode banner does), and its regex does not match "ERROR: <403>" either.
    """
    return [int(code) for code in re.findall(r"ERROR:\s*<(\d+)>", stdout or "")]


def test_traversal_answers_do_not_leak_existence(env: SecurityTestEnvironment, results: TestResult):
    """A path outside the root must answer the same whether or not it exists.

    Traversal was already blocked, but the *error code* used to differ: handlers asked the
    filesystem whether the path existed before checking containment, so "../../x" answered 403 when
    x was there and 400/412 when it was not. No file escaped, yet the difference alone let a caller
    enumerate arbitrary paths on the server - /etc/shadow, someone's ~/.ssh/id_rsa - one probe at a
    time. guard_path() decides containment first, so every handler now answers identically.
    """
    present = os.path.join(env.server_root, "oracle_probe_present.txt")
    with open(present, "w") as f:
        f.write("a file that exists outside every user root")
    absent = "oracle_probe_absent.txt"
    if os.path.exists(os.path.join(env.server_root, absent)):
        os.remove(os.path.join(env.server_root, absent))

    # One entry per handler that takes a path, single- and two-argument forms alike
    probes = [
        ("DELETE", "DELETE ../../{}"),
        ("DOWNLOAD", "DOWNLOAD ../../{} out.bin"),
        ("LIST", "LIST ../../{}"),
        ("CD", "CD ../../{}"),
        ("MKDIR", "MKDIR ../../{}"),
        ("RMDIR", "RMDIR ../../{}"),
        ("MOVE", "MOVE ../../{} moved.txt"),
        ("COPY", "COPY ../../{} copied.txt"),
        ("SYNC", "SYNC . ../../{}"),
    ]

    leaked = []
    for name, template in probes:
        stdout_present, _, _ = env.run_public(
            template.format("oracle_probe_present.txt"), f"oracle_{name.lower()}_present")
        stdout_absent, _, _ = env.run_public(
            template.format(absent), f"oracle_{name.lower()}_absent")

        codes_present = error_codes(stdout_present)
        codes_absent = error_codes(stdout_absent)

        if not codes_present or not codes_absent:
            leaked.append(f"{name}: expected an error for both, got {codes_present} / {codes_absent}")
        elif codes_present != codes_absent:
            leaked.append(f"{name}: exists={codes_present} absent={codes_absent}")

    if leaked:
        results.fail("Traversal errors do not leak existence", "; ".join(leaked))
        return
    results.ok("Traversal errors do not leak existence")

    with open(present) as f:
        if f.read() != "a file that exists outside every user root":
            results.fail("Traversal leaves outside files alone", "the probe file was modified")
            return
    results.ok("Traversal leaves outside files alone")


def test_auth_lockout_after_repeated_failures(env: SecurityTestEnvironment, results: TestResult):
    """Password guessing must be rate limited, and the limit must survive reconnecting."""
    username = "lockout_target"
    password = "correct_horse_battery"
    env.register_user(username, password, "register_lockout_target")

    # Five wrong guesses in one session: the last must be refused rather than merely rejected
    stdout, _, _ = env._run_client(
        f"{username}@127.0.0.1:{env.port}",
        ["wrong1", "wrong2", "wrong3", "wrong4", "wrong5", "EXIT"],
        "lockout_burst",
    )
    if 429 not in error_codes(stdout):
        results.fail("Repeated password failures lock the account",
                     f"expected a 429 within one session, got {error_codes(stdout)}")
        return
    results.ok("Repeated password failures lock the account")

    # A fresh connection must not hand the attacker a fresh allowance
    stdout, _, _ = env._run_client(
        f"{username}@127.0.0.1:{env.port}", ["wrong6", "EXIT"], "lockout_reconnect")
    if 429 not in error_codes(stdout):
        results.fail("The lockout survives reconnecting",
                     f"a new connection was allowed to guess again: {error_codes(stdout)}")
        return
    results.ok("The lockout survives reconnecting")

    # ...including for the real password, which is what makes it a lockout rather than a delay
    stdout, _, _ = env._run_client(
        f"{username}@127.0.0.1:{env.port}", [password, "EXIT"], "lockout_blocks_correct")
    if 429 not in error_codes(stdout):
        results.fail("The lockout applies to the correct password too", stdout[:200])
        return
    results.ok("The lockout applies to the correct password too")


def test_lockout_cannot_be_held_open_by_an_attacker(env: SecurityTestEnvironment, results: TestResult):
    """Guessing during a lockout must not extend it.

    A per-username lockout is also a denial-of-service primitive: if every attempt pushed the
    deadline back, an attacker who knows a username could keep its owner permanently locked out,
    which is worse than the unlimited retries this replaced. check() runs before the password is
    verified, so a refused attempt never counts as a failure and the deadline only moves forward
    when a *permitted* attempt fails.
    """
    username = "dos_target"
    password = "another_good_password"
    env.register_user(username, password, "register_dos_target")

    env._run_client(f"{username}@127.0.0.1:{env.port}",
                    ["no1", "no2", "no3", "no4", "no5", "EXIT"], "dos_initial_lockout")

    def remaining():
        stdout, _, _ = env._run_client(
            f"{username}@127.0.0.1:{env.port}", ["still_guessing", "EXIT"], "dos_probe")
        found = re.search(r"in (\d+) second", stdout or "")
        return int(found.group(1)) if found else None

    first = remaining()
    if first is None:
        results.fail("An attacker cannot hold the lockout open", "no lockout was in effect to probe")
        return

    time.sleep(6)
    second = remaining()
    if second is None:
        # The lockout expired while being hammered, which is also a pass: it did not get extended
        results.ok("An attacker cannot hold the lockout open")
    elif second >= first:
        results.fail("An attacker cannot hold the lockout open",
                     f"remaining time went {first}s -> {second}s while guessing continued")
        return
    else:
        results.ok("An attacker cannot hold the lockout open")

    # And once it lapses, the real owner gets in
    deadline = time.time() + 60
    while time.time() < deadline:
        stdout, _, _ = env._run_client(
            f"{username}@127.0.0.1:{env.port}", [password, "EXIT"], "dos_recover")
        if 429 not in error_codes(stdout):
            results.ok("The real owner can log in once the lockout lapses")
            return
        time.sleep(5)
    results.fail("The real owner can log in once the lockout lapses", "still locked out after 60s")


def test_failed_command_does_not_strand_the_user_lock(env: SecurityTestEnvironment, results: TestResult):
    """Every error path must release the per-user lock.

    The lock is a non-reentrant busy flag, so a handler that returns without releasing makes every
    later command from that user fail with "Server is busy" until the server restarts - and from
    the client it looks exactly like a deadlock. There were ~50 hand-written release paths; UserLock
    replaced them with RAII. This drives each handler down a rejected path and then checks the user
    can still work.
    """
    username = "lockuser"
    password = "lockuser_pw"
    env.register_user(username, password, "register_lockuser")

    # One rejected request per locking handler, then an ordinary command that must succeed
    stdout, _, work = env.run_user(username, password, [
        "MKDIR ../../escape",          # 403 from guard_path
        "DELETE ../../escape",         # 403
        "RMDIR nonexistent_dir",       # 400
        "MOVE nope.txt also_nope.txt", # 412
        "COPY nope.txt also_nope.txt", # 412
        "SYNC . ../../escape",         # 403
        "DOWNLOAD nonexistent.bin",    # 412
        "UPLOAD /nonexistent/local/path.bin remote.bin",
        "LIST ../../escape",           # 403
        "MKDIR survivor",              # must work: the lock was released every time
        "LIST",
    ], "lock_release_paths", timeout=60)

    if "Server is busy" in (stdout or ""):
        results.fail("Rejected commands release the user lock",
                     "a later command was refused as busy, so a handler kept the lock")
        return

    if not os.path.isdir(os.path.join(env.server_root, "private", username, "files", "survivor")):
        results.fail("Rejected commands release the user lock",
                     f"the follow-up MKDIR did not take effect: {stdout[-300:]!r}")
        return
    results.ok("Rejected commands release the user lock")


def main():
    print("MiniDrive Integration Tests - Security")
    print("=" * 60)

    check_executables()

    env = SecurityTestEnvironment("security", SERVER_PORT)
    results = TestResult(env.log_dir)

    try:
        env.setup_server_root()
        env.start_server()

        test_public_path_traversal_download_blocked(env, results)
        test_user_cannot_access_other_user_private(env, results)
        test_traversal_answers_do_not_leak_existence(env, results)
        test_failed_command_does_not_strand_the_user_lock(env, results)
        test_auth_lockout_after_repeated_failures(env, results)
        test_lockout_cannot_be_held_open_by_an_attacker(env, results)
        test_users_db_has_no_plaintext_password(env, results)

    finally:
        env.cleanup()

    ok = results.summary()
    print(f"\nLogs saved to: {env.log_dir}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
