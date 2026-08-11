#!/usr/bin/env python3
"""Integration tests for per-user storage tiering.

The server is started with two storage media declared via --tier, on top of a
separate control root that holds users.json and public/. These tests check that:

- TIERS advertises the configured media by name and description, and never leaks
  a server filesystem path.
- SET_TIER refuses a medium the server was not configured with.
- Confirming SET_TIER physically relocates the user's files to the other medium
  with their content intact, and cancelling it leaves everything alone.
- The path traversal guard still binds to the user's root after a migration.
- users.json records the tier, and a legacy users.json without the field still loads.
- A server started with only --root behaves exactly as it always has.

Assertions look at filesystem state rather than has_ok_response(), which matches
the bare substring "ok" anywhere in a transcript (including the login banner).

Usage:
    python3 tests/integration/test_storage_tiers.py
"""

import os
import re
import sys
import json
import shutil
import hashlib
import subprocess
import time

from test_utils import (
    TestEnvironment,
    TestResult,
    check_executables,
    SERVER_EXE,
)

SERVER_PORT = 9030


def error_codes(stdout: str):
    """All protocol error codes in a transcript, as the client actually prints them.

    test_utils.get_error_code() is unusable here: it returns None as soon as the
    substring "ok" appears anywhere earlier in the output, which every private-mode
    login does ("OK: Authentication successful."), and its regex does not match the
    client's "ERROR: <404>" format either.
    """
    return [int(code) for code in re.findall(r"ERROR:\s*<(\d+)>", stdout or "")]


class TierTestEnvironment(TestEnvironment):
    """Server with a control root plus two declared storage media."""

    def __init__(self, suite_name: str, port: int):
        super().__init__(suite_name, port)
        self.hot_root = os.path.abspath(f"data/test_{suite_name}_hot")
        self.archive_root = os.path.abspath(f"data/test_{suite_name}_archive")

    def setup_server_root(self):
        super().setup_server_root()
        for root in (self.hot_root, self.archive_root):
            if os.path.exists(root):
                shutil.rmtree(root)
            os.makedirs(root, exist_ok=True)

    def start_server(self):
        self.server_log_file = open(os.path.join(self.log_dir, "server.log"), "a")
        self.server_process = subprocess.Popen(
            [
                SERVER_EXE, "--port", str(self.port), "--root", self.server_root,
                "--tier", f"hot={self.hot_root}",
                "--tier-desc", "hot=NVMe SSD, fast",
                "--tier", f"archive={self.archive_root}",
                "--tier-desc", "archive=7200rpm HDD, bulk",
                "--default-tier", "hot",
            ],
            stdout=self.server_log_file,
            stderr=self.server_log_file,
            text=True,
        )
        time.sleep(0.5)
        if self.server_process.poll() is not None:
            self.server_log_file.close()
            with open(os.path.join(self.log_dir, "server.log")) as f:
                output = f.read()
            raise RuntimeError(f"Server failed to start: {output}")
        print(f"Server started on port {self.port} (PID: {self.server_process.pid})")

    def _run_client(self, connect_arg, input_lines, test_name, timeout=60):
        self.test_counter += 1
        log_prefix = f"{self.test_counter:02d}_{test_name}"
        stdout_log = os.path.join(self.log_dir, f"{log_prefix}_stdout.log")
        work_dir = os.path.join(self.log_dir, f"{log_prefix}_workdir")
        os.makedirs(work_dir, exist_ok=True)

        process = self.start_client_process(
            connect_arg, os.path.join(self.log_dir, f"{log_prefix}_client.log"), cwd=work_dir
        )
        try:
            stdout, _ = process.communicate(input="\n".join(input_lines) + "\n", timeout=timeout)
        except subprocess.TimeoutExpired:
            process.kill()
            with open(stdout_log, "w") as f:
                f.write(f"# TIMEOUT after {timeout}s\n")
            return "TIMEOUT", work_dir

        with open(stdout_log, "w") as f:
            f.write(f"# Connect: {connect_arg}\n# Input: {input_lines}\n{'='*50}\n\n")
            f.write(stdout)
        return stdout, work_dir

    def register(self, username, password, test_name):
        """Register a user: the server asks to register, then asks for a password."""
        return self._run_client(
            f"{username}@127.0.0.1:{self.port}", ["y", password, "EXIT"], test_name
        )

    def run_user(self, username, password, commands, test_name, timeout=60):
        if isinstance(commands, str):
            commands = [commands]
        return self._run_client(
            f"{username}@127.0.0.1:{self.port}", [password] + commands + ["EXIT"], test_name, timeout
        )

    def run_public(self, commands, test_name, timeout=60):
        if isinstance(commands, str):
            commands = [commands]
        return self._run_client(f"127.0.0.1:{self.port}", commands + ["EXIT"], test_name, timeout)

    def user_files_dir(self, root, username):
        return os.path.join(root, "private", username, "files")

    def read_users_db(self):
        with open(os.path.join(self.server_root, "users.json")) as f:
            return json.load(f)

    def stored_tier(self, username):
        for user in self.read_users_db().get("users", []):
            if user.get("username") == username:
                return user.get("storage_class")
        return None

    def cleanup(self):
        super().cleanup()
        for root in (getattr(self, "hot_root", None), getattr(self, "archive_root", None)):
            if root and os.path.exists(root):
                shutil.rmtree(root, ignore_errors=True)


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(65536), b""):
            h.update(block)
    return h.hexdigest()


def upload_probe(env, username, password, work_files, test_name):
    """Upload a set of {remote_name: content} for a user, from a scratch directory."""
    scratch = os.path.join(env.log_dir, f"scratch_{test_name}")
    os.makedirs(scratch, exist_ok=True)
    commands = []
    for name, content in work_files.items():
        local = os.path.join(scratch, name)
        with open(local, "w") as f:
            f.write(content)
        commands.append(f"UPLOAD {local} {name}")
    return env.run_user(username, password, commands, test_name)


# --------------------------------------------------------------------------- tests


def test_tiers_lists_configured_media(env, results):
    env.register("lister", "pw_lister", "register_lister")
    stdout, _ = env.run_user("lister", "pw_lister", "TIERS", "tiers_listing")

    if "hot" not in stdout or "archive" not in stdout:
        results.fail("TIERS lists configured media", f"missing tier names in output: {stdout!r}")
        return
    results.ok("TIERS lists configured media")

    if "NVMe SSD, fast" not in stdout or "7200rpm HDD, bulk" not in stdout:
        results.fail("TIERS includes descriptions", "descriptions missing from output")
        return
    results.ok("TIERS includes descriptions")

    if "(current)" not in stdout:
        results.fail("TIERS marks the current tier", "no current tier marker in output")
        return
    results.ok("TIERS marks the current tier")


def test_tiers_does_not_leak_server_paths(env, results):
    stdout, _ = env.run_user("lister", "pw_lister", "TIERS", "tiers_no_paths")
    for root in (env.hot_root, env.archive_root, env.server_root):
        if root in stdout:
            results.fail("TIERS does not leak server paths", f"output contained {root}")
            return
    results.ok("TIERS does not leak server paths")


def test_new_user_defaults_to_hot(env, results):
    env.register("alice", "pw_alice", "register_alice")

    if env.stored_tier("alice") != "hot":
        results.fail("New user defaults to hot tier",
                     f"users.json records {env.stored_tier('alice')!r}")
        return
    results.ok("New user defaults to hot tier")

    if not os.path.isdir(env.user_files_dir(env.hot_root, "alice")):
        results.fail("New user directory is on the hot medium", "files dir missing under hot root")
        return
    if os.path.exists(os.path.join(env.archive_root, "private", "alice")):
        results.fail("New user directory is on the hot medium", "directory also present on archive")
        return
    results.ok("New user directory is on the hot medium")


def test_unknown_tier_is_rejected(env, results):
    before = env.stored_tier("alice")
    stdout, _ = env.run_user("alice", "pw_alice", "SET_TIER nonexistent_medium", "unknown_tier")

    if 404 not in error_codes(stdout):
        results.fail("Unknown tier is rejected with 404", f"got error codes {error_codes(stdout)}")
        return
    results.ok("Unknown tier is rejected with 404")

    if env.stored_tier("alice") != before:
        results.fail("Unknown tier leaves the record alone",
                     f"tier changed from {before!r} to {env.stored_tier('alice')!r}")
        return
    results.ok("Unknown tier leaves the record alone")


def test_set_tier_to_current_is_a_noop(env, results):
    stdout, _ = env.run_user("alice", "pw_alice", "SET_TIER hot", "same_tier")
    if error_codes(stdout):
        results.fail("SET_TIER to the current tier succeeds", f"got errors {error_codes(stdout)}")
        return
    if "Already on tier" not in stdout:
        results.fail("SET_TIER to the current tier succeeds", "no no-op message in output")
        return
    results.ok("SET_TIER to the current tier succeeds")


def test_cancelled_migration_changes_nothing(env, results):
    upload_probe(env, "alice", "pw_alice", {"keep.txt": "cancel me"}, "upload_before_cancel")

    hot_file = os.path.join(env.user_files_dir(env.hot_root, "alice"), "keep.txt")
    if not os.path.exists(hot_file):
        results.fail("Cancelled migration setup", "upload did not land on the hot medium")
        return

    env.run_user("alice", "pw_alice", ["SET_TIER archive", "n"], "cancel_migration")

    if not os.path.exists(hot_file):
        results.fail("Cancelled migration leaves data in place", "file left the hot medium")
        return
    if os.path.exists(os.path.join(env.archive_root, "private", "alice")):
        results.fail("Cancelled migration leaves data in place", "data appeared on archive")
        return
    if env.stored_tier("alice") != "hot":
        results.fail("Cancelled migration leaves the record alone",
                     f"users.json now records {env.stored_tier('alice')!r}")
        return
    results.ok("Cancelled migration leaves data in place")
    results.ok("Cancelled migration leaves the record alone")


def test_migration_moves_data_between_media(env, results):
    payload = "tiered payload " * 500
    upload_probe(env, "alice", "pw_alice", {"moved.txt": payload}, "upload_before_move")

    hot_file = os.path.join(env.user_files_dir(env.hot_root, "alice"), "moved.txt")
    if not os.path.exists(hot_file):
        results.fail("Migration setup", "upload did not land on the hot medium")
        return
    expected_hash = md5(hot_file)

    stdout, _ = env.run_user("alice", "pw_alice", ["SET_TIER archive", "y"], "migrate_to_archive")

    if error_codes(stdout):
        results.fail("Migration reports success", f"got errors {error_codes(stdout)}")
        return
    if "Moved to tier 'archive'" not in stdout:
        results.fail("Migration reports success", "no confirmation message in output")
        return
    results.ok("Migration reports success")

    archive_file = os.path.join(env.user_files_dir(env.archive_root, "alice"), "moved.txt")
    if not os.path.exists(archive_file):
        results.fail("Migration places data on the target medium", "file missing under archive root")
        return
    results.ok("Migration places data on the target medium")

    if md5(archive_file) != expected_hash:
        results.fail("Migration preserves file contents", "hash mismatch after the move")
        return
    results.ok("Migration preserves file contents")

    if os.path.exists(os.path.join(env.hot_root, "private", "alice")):
        results.fail("Migration removes data from the source medium",
                     "the old copy is still on the hot root")
        return
    results.ok("Migration removes data from the source medium")

    if env.stored_tier("alice") != "archive":
        results.fail("Migration records the new tier",
                     f"users.json records {env.stored_tier('alice')!r}")
        return
    results.ok("Migration records the new tier")


def test_files_are_usable_after_migration(env, results):
    stdout, work_dir = env.run_user(
        "alice", "pw_alice", ["LIST", "DOWNLOAD moved.txt pulled.txt"], "use_after_migration"
    )

    if "moved.txt" not in stdout:
        results.fail("LIST works after migration", "moved.txt missing from listing")
        return
    results.ok("LIST works after migration")

    pulled = os.path.join(work_dir, "pulled.txt")
    archive_file = os.path.join(env.user_files_dir(env.archive_root, "alice"), "moved.txt")
    if not os.path.exists(pulled) or md5(pulled) != md5(archive_file):
        results.fail("DOWNLOAD works after migration", "downloaded file missing or corrupted")
        return
    results.ok("DOWNLOAD works after migration")

    stdout, _ = env.run_user("alice", "pw_alice", "TIERS", "tiers_after_migration")
    current_line = [line for line in stdout.splitlines() if "(current)" in line]
    if not current_line or "archive" not in current_line[0]:
        results.fail("TIERS reflects the new tier", f"current marker on {current_line!r}")
        return
    results.ok("TIERS reflects the new tier")


def test_traversal_still_blocked_after_migration(env, results):
    stdout, work_dir = env.run_user(
        "alice", "pw_alice",
        ["CD ../..", "DOWNLOAD ../../users.json stolen.json", "DOWNLOAD ../../../users.json stolen2.json"],
        "traversal_after_migration",
    )

    if 403 not in error_codes(stdout):
        results.fail("Traversal is refused after migration",
                     f"CD outside the root was not refused, got {error_codes(stdout)}")
        return
    for leaked in ("stolen.json", "stolen2.json"):
        if os.path.exists(os.path.join(work_dir, leaked)):
            results.fail("Traversal is refused after migration", f"{leaked} was written")
            return
    results.ok("Traversal is refused after migration")


def test_public_mode_has_no_tier(env, results):
    stdout, _ = env.run_public(["SET_TIER archive"], "public_set_tier")
    if 403 not in error_codes(stdout):
        results.fail("Public mode cannot set a tier", f"got error codes {error_codes(stdout)}")
        return
    results.ok("Public mode cannot set a tier")


def test_legacy_users_db_without_storage_class(env, results):
    """A users.json written before tiering existed must still load and log in."""
    env.register("legacy", "pw_legacy", "register_legacy")
    upload_probe(env, "legacy", "pw_legacy", {"old.txt": "written before tiering"}, "upload_legacy")

    env.stop_server()

    db_path = os.path.join(env.server_root, "users.json")
    with open(db_path) as f:
        db = json.load(f)
    for user in db["users"]:
        user.pop("storage_class", None)
    with open(db_path, "w") as f:
        json.dump(db, f, indent=4)

    env.start_server()

    stdout, _ = env.run_user("legacy", "pw_legacy", "LIST", "legacy_login")
    if "old.txt" not in stdout:
        results.fail("Legacy users.json still works", f"file not listed after reload: {stdout!r}")
        return
    results.ok("Legacy users.json still works")


def test_single_root_server_is_unchanged(env, results):
    """A server started the classic way, with only --root, must behave as before."""
    root = os.path.abspath("data/test_tiers_plain_root")
    work = os.path.abspath("data/test_tiers_plain_cwd")
    for path in (root, work):
        if os.path.exists(path):
            shutil.rmtree(path)
        os.makedirs(path, exist_ok=True)

    log = open(os.path.join(env.log_dir, "server_singleroot.log"), "w")
    port = env.port + 1
    proc = subprocess.Popen(
        [SERVER_EXE, "--port", str(port), "--root", root], stdout=log, stderr=log, text=True
    )
    try:
        time.sleep(0.7)
        if proc.poll() is not None:
            results.fail("Single-root server still starts", "server exited immediately")
            return

        local = os.path.join(work, "plain.txt")
        with open(local, "w") as f:
            f.write("classic single root layout")

        client = env.start_client_process(f"bob@127.0.0.1:{port}", None, cwd=work)
        stdout, _ = client.communicate(
            input=f"y\npw_bob\nUPLOAD {local} plain.txt\nLIST\nEXIT\n", timeout=60
        )

        expected = os.path.join(root, "private", "bob", "files", "plain.txt")
        if not os.path.exists(expected):
            results.fail("Single-root server keeps the classic layout",
                         f"expected {expected}, transcript: {stdout!r}")
            return
        results.ok("Single-root server keeps the classic layout")
    except subprocess.TimeoutExpired:
        results.fail("Single-root server keeps the classic layout", "client timed out")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()
        for path in (root, work):
            shutil.rmtree(path, ignore_errors=True)


def main():
    print("MiniDrive Integration Tests - Storage Tiering")
    print("=" * 60)

    check_executables()

    env = TierTestEnvironment("tiers", SERVER_PORT)
    results = TestResult(env.log_dir)

    try:
        env.setup_server_root()
        env.start_server()

        test_tiers_lists_configured_media(env, results)
        test_tiers_does_not_leak_server_paths(env, results)
        test_new_user_defaults_to_hot(env, results)
        test_unknown_tier_is_rejected(env, results)
        test_set_tier_to_current_is_a_noop(env, results)
        test_cancelled_migration_changes_nothing(env, results)
        test_migration_moves_data_between_media(env, results)
        test_files_are_usable_after_migration(env, results)
        test_traversal_still_blocked_after_migration(env, results)
        test_public_mode_has_no_tier(env, results)
        test_legacy_users_db_without_storage_class(env, results)
        test_single_root_server_is_unchanged(env, results)

    finally:
        env.cleanup()

    ok = results.summary()
    print(f"\nLogs saved to: {env.log_dir}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
