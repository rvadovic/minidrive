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
        test_users_db_has_no_plaintext_password(env, results)

    finally:
        env.cleanup()

    ok = results.summary()
    print(f"\nLogs saved to: {env.log_dir}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
