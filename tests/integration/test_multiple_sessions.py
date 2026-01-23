#!/usr/bin/env python3
"""Integration tests for MiniDrive Multiple Sessions.

Goal:
- Validate the server can handle multiple sessions for the same user (or public mode).
- Exercise concurrent operations on the same files/user.

Usage:
    python3 tests/integration/test_multiple_sessions.py
"""

import os
import sys
import subprocess
import time
import signal
import shutil
import threading
import re

from test_utils import (
    TestResult,
    check_executables,
    SERVER_EXE,
    CLIENT_EXE,
    get_test_log_dir,
    calculate_hash,
    has_ok_response,
    TestEnvironment,
    communicate_and_log
)

SERVER_PORT = 9029


class MultiSessionEnv(TestEnvironment):

    def is_server_alive(self) -> bool:
        return self.server_process is not None and self.server_process.poll() is None

    def spawn_client(self, cwd: str, commands, label: str):
        """Spawn client process in its own cwd and return (proc, stdout_log_path)."""
        if isinstance(commands, list):
            input_lines = commands + ["EXIT"]
        else:
            input_lines = [commands, "EXIT"]

        stdout_log = os.path.join(self.log_dir, f"{label}_stdout.log")
        client_log = os.path.join(self.log_dir, f"{label}_client.log")
        proc = self.start_client_process(f"127.0.0.1:{self.port}", client_log, cwd=cwd)
        input_data = "\n".join(input_lines) + "\n"
        return proc, input_data, stdout_log

    def spawn_auth_client(self, cwd: str, username: str, password: str, commands, label: str):
        """Spawn authenticated client process and return (proc, input_data, stdout_log_path)."""
        if isinstance(commands, list):
            input_lines = [password] + commands + ["EXIT"]
        else:
            input_lines = [password, commands, "EXIT"]

        stdout_log = os.path.join(self.log_dir, f"{label}_stdout.log")
        client_log = os.path.join(self.log_dir, f"{label}_client.log")
        proc = self.start_client_process(f"{username}@127.0.0.1:{self.port}", client_log, cwd=cwd)
        input_data = "\n".join(input_lines) + "\n"
        return proc, input_data, stdout_log

    def cleanup(self):
        self.stop_server()
        if os.path.exists(self.server_root):
            shutil.rmtree(self.server_root, ignore_errors=True)


def _run_parallel_jobs(jobs):
    """Run multiple callables in parallel threads and return their results."""
    results = [None] * len(jobs)

    def _runner(i, fn):
        results[i] = fn()

    threads = [threading.Thread(target=_runner, args=(i, fn), daemon=True) for i, fn in enumerate(jobs)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    return results


_JSON_OK_RE = re.compile(r"\"status\"\s*:\s*\"OK\"")


def verify_registration_login(env: MultiSessionEnv, username: str, password: str, label: str) -> bool:
    """Verify registration by logging in and running LIST."""
    cwd = env.new_workdir(f"{label}_verify")
    # Just run LIST
    proc, input_data, stdout_log = env.spawn_auth_client(cwd, username, password, "LIST", f"{label}_verify")
    stdout, code = communicate_and_log(proc, input_data, stdout_log)
    return has_ok_response(stdout)


def test_concurrent_downloads_public(env: MultiSessionEnv, results: TestResult):
    """Multiple public clients download the same server file concurrently."""
    server_name = "concurrent_download.bin"
    
    # Upload the file first using a client to ensure it's in the right place
    cwd = env.new_workdir("setup_download")
    local_path = os.path.join(cwd, server_name)
    data = os.urandom(2 * 1024 * 1024)  # 2MB
    with open(local_path, "wb") as f:
        f.write(data)
        
    proc, input_data, stdout_log = env.spawn_client(cwd, f"UPLOAD {server_name} {server_name}", "setup_download")
    stdout, code = communicate_and_log(proc, input_data, stdout_log)
    
    if not has_ok_response(stdout):
        results.fail("Concurrent downloads (public)", "Failed to setup test file")
        return

    # Calculate expected hash from local file
    expected_hash = calculate_hash(local_path)

    procs = []
    for i in range(4):
        cwd = env.new_workdir(f"download_{i}")
        out_name = f"dl_{i}.bin"
        cmd = f"DOWNLOAD {server_name} {out_name}"
        proc, input_data, stdout_log = env.spawn_client(cwd, cmd, f"download_{i}")
        procs.append((proc, input_data, stdout_log, cwd, out_name))

    # Start all clients before waiting.
    results.ok("Spawned concurrent download clients (public)")

    jobs = []
    for proc, input_data, stdout_log, cwd, out_name in procs:
        def _make_job(p=proc, inp=input_data, log_path=stdout_log, work=cwd, out=out_name):
            def _job():
                stdout, code = communicate_and_log(p, inp, log_path, timeout=60)
                return stdout, code, log_path, work, out

            return _job

        jobs.append(_make_job())

    run_results = _run_parallel_jobs(jobs)

    failures = []
    for stdout, code, stdout_log, cwd, out_name in run_results:
        out_path = os.path.join(cwd, out_name)
        if not has_ok_response(stdout):
            failures.append(f"Client did not return OK. See {stdout_log}")
            continue
        if not os.path.exists(out_path):
            failures.append(f"Missing downloaded file: {out_path}")
            continue
        got_hash = calculate_hash(out_path)
        if got_hash != expected_hash:
            failures.append(f"Hash mismatch in downloaded file: {out_path}")

    if failures:
        results.fail("Concurrent downloads (public)", failures[0])
        return

    results.ok("Concurrent downloads (public) produce correct content")


def test_concurrent_uploads_public(env: MultiSessionEnv, results: TestResult):
    """Multiple public clients upload different files concurrently."""
    procs = []
    payloads = {}

    for i in range(4):
        cwd = env.new_workdir(f"upload_{i}")
        local_name = f"up_{i}.bin"
        remote_name = f"remote_up_{i}.bin"
        local_path = os.path.join(cwd, local_name)

        content = (f"client-{i}-".encode("utf-8") + os.urandom(256 * 1024))  # ~256KB
        payloads[remote_name] = content

        with open(local_path, "wb") as f:
            f.write(content)

        cmd = f"UPLOAD {local_name} {remote_name}"
        proc, input_data, stdout_log = env.spawn_client(cwd, cmd, f"upload_{i}")
        procs.append((proc, input_data, stdout_log))

    results.ok("Spawned concurrent upload clients (public)")

    for proc, input_data, stdout_log in procs:
        stdout, code = communicate_and_log(proc, input_data, stdout_log, timeout=60)
        if not has_ok_response(stdout):
            results.fail("Concurrent uploads (public)", f"Client did not return OK. See {stdout_log}")
            return

    # Validate by downloading back
    for remote_name, expected in payloads.items():
        cwd = env.new_workdir(f"verify_{remote_name}")
        download_name = f"downloaded_{remote_name}"
        cmd = f"DOWNLOAD {remote_name} {download_name}"
        proc, input_data, stdout_log = env.spawn_client(cwd, cmd, f"verify_{remote_name}")
        stdout, code = communicate_and_log(proc, input_data, stdout_log, timeout=30)
        
        if not has_ok_response(stdout):
            results.fail("Concurrent uploads (public)", f"Failed to download verification file: {remote_name}")
            return
            
        download_path = os.path.join(cwd, download_name)
        if not os.path.exists(download_path):
            results.fail("Concurrent uploads (public)", f"Downloaded file missing: {download_path}")
            return
            
        with open(download_path, "rb") as f:
            got = f.read()
        if got != expected:
            results.fail("Concurrent uploads (public)", f"Content mismatch for {remote_name}")
            return

    results.ok("Concurrent uploads (public) persist correct content")


def test_mixed_upload_download_public(env: MultiSessionEnv, results: TestResult):
    """Upload and download in parallel (public); server should remain responsive."""
    # Prepare a download source
    server_name = "mix_source.bin"
    
    # Upload setup file
    cwd = env.new_workdir("setup_mixed")
    local_path = os.path.join(cwd, server_name)
    with open(local_path, "wb") as f:
        f.write(os.urandom(1024 * 1024))
        
    proc, input_data, stdout_log = env.spawn_client(cwd, f"UPLOAD {server_name} {server_name}", "setup_mixed")
    communicate_and_log(proc, input_data, stdout_log)
    
    expected_hash = calculate_hash(local_path)

    procs = []

    # Two downloaders
    for i in range(2):
        cwd = env.new_workdir(f"mix_dl_{i}")
        out_name = f"mix_dl_{i}.bin"
        cmd = f"DOWNLOAD {server_name} {out_name}"
        proc, input_data, stdout_log = env.spawn_client(cwd, cmd, f"mix_dl_{i}")
        procs.append(("dl", proc, input_data, stdout_log, cwd, out_name))

    # Two uploaders
    for i in range(2):
        cwd = env.new_workdir(f"mix_up_{i}")
        local_name = f"mix_up_{i}.bin"
        remote_name = f"mix_remote_{i}.bin"
        local_path = os.path.join(cwd, local_name)
        content = os.urandom(512 * 1024)
        with open(local_path, "wb") as f:
            f.write(content)
        cmd = f"UPLOAD {local_name} {remote_name}"
        proc, input_data, stdout_log = env.spawn_client(cwd, cmd, f"mix_up_{i}")
        procs.append(("up", proc, input_data, stdout_log, remote_name, content))

    results.ok("Spawned mixed upload/download clients (public)")

    # Collect results
    uploaded = {}
    for kind, proc, input_data, stdout_log, a, b in procs:
        stdout, code = communicate_and_log(proc, input_data, stdout_log, timeout=60)
        if not has_ok_response(stdout):
            results.fail("Mixed concurrency (public)", f"Client did not return OK. See {stdout_log}")
            return
        if kind == "dl":
            cwd, out_name = a, b
            out_path = os.path.join(cwd, out_name)
            if not os.path.exists(out_path):
                results.fail("Mixed concurrency (public)", f"Missing downloaded file: {out_path}")
                return
            if calculate_hash(out_path) != expected_hash:
                results.fail("Mixed concurrency (public)", "Downloaded content hash mismatch")
                return
        else:
            remote_name, content = a, b
            uploaded[remote_name] = content

    for remote_name, content in uploaded.items():
        cwd = env.new_workdir(f"verify_mix_{remote_name}")
        download_name = f"downloaded_{remote_name}"
        cmd = f"DOWNLOAD {remote_name} {download_name}"
        proc, input_data, stdout_log = env.spawn_client(cwd, cmd, f"verify_mix_{remote_name}")
        stdout, code = communicate_and_log(proc, input_data, stdout_log, timeout=30)
        
        if not has_ok_response(stdout):
            results.fail("Mixed concurrency (public)", f"Failed to download verification file: {remote_name}")
            return
            
        download_path = os.path.join(cwd, download_name)
        if not os.path.exists(download_path):
            results.fail("Mixed concurrency (public)", f"Downloaded file missing: {download_path}")
            return
            
        with open(download_path, "rb") as f:
            got = f.read()
        if got != content:
            results.fail("Mixed concurrency (public)", f"Content mismatch for {remote_name}")
            return

    # Server should still be alive
    if not env.is_server_alive():
        results.fail("Mixed concurrency (public)", "Server process died during mixed operations")
        return

    results.ok("Mixed upload/download (public) leaves server responsive")


def test_race_condition_upload(env: MultiSessionEnv, results: TestResult):
    """Multiple public clients upload the SAME file concurrently.
    
    One should succeed, or both, but server must not crash.
    The final file content should match one of the uploads.
    """
    remote_name = "race_upload.bin"
    
    # Prepare two different contents for the same filename
    content1 = b"A" * (1024 * 1024) # 1MB of A
    content2 = b"B" * (1024 * 1024) # 1MB of B
    
    cwd1 = env.new_workdir("race_1")
    local_path1 = os.path.join(cwd1, remote_name)
    with open(local_path1, "wb") as f:
        f.write(content1)
        
    cwd2 = env.new_workdir("race_2")
    local_path2 = os.path.join(cwd2, remote_name)
    with open(local_path2, "wb") as f:
        f.write(content2)
        
    # Spawn both uploads
    proc1, input1, log1 = env.spawn_client(cwd1, f"UPLOAD {remote_name}", "race_1")
    proc2, input2, log2 = env.spawn_client(cwd2, f"UPLOAD {remote_name}", "race_2")
    
    results.ok("Spawned conflicting uploads")
    
    # Run them
    def run_client(p, i, l):
        return communicate_and_log(p, i, l, timeout=60)
        
    jobs = [
        lambda: run_client(proc1, input1, log1),
        lambda: run_client(proc2, input2, log2)
    ]
    
    run_results = _run_parallel_jobs(jobs)
    
    # Check results
    # It is acceptable if one fails, or both succeed (overwrite).
    # It is NOT acceptable if server crashes.
    
    if not env.is_server_alive():
        results.fail("Race condition upload", "Server crashed during conflicting uploads")
        return
        
    # Verify the file on server matches one of them
    cwd_verify = env.new_workdir("race_verify")
    download_name = "race_result.bin"
    proc, input_data, stdout_log = env.spawn_client(cwd_verify, f"DOWNLOAD {remote_name} {download_name}", "race_verify")
    stdout, code = communicate_and_log(proc, input_data, stdout_log)
    
    if not has_ok_response(stdout):
        results.fail("Race condition upload", "Failed to download result file")
        return
        
    download_path = os.path.join(cwd_verify, download_name)
    if not os.path.exists(download_path):
        results.fail("Race condition upload", "Result file missing")
        return
        
    with open(download_path, "rb") as f:
        final_content = f.read()
        
    if final_content == content1:
        results.ok("Race condition handled (Content A persisted)")
    elif final_content == content2:
        results.ok("Race condition handled (Content B persisted)")
    else:
        results.fail("Race condition upload", "File corrupted (content matches neither A nor B)")


def test_auth_multi_session_consistency(env: MultiSessionEnv, results: TestResult):
    """Verify consistency between multiple sessions of the same authenticated user."""
    username = "multi_session_user"
    password = "password"
    stdout, code = env.register_user(username, password, "multi_sess_reg")
    if code != 0:
        results.fail("Auth Multi-Session", "Registration failed")
        return

    # Client A uploads
    cwd_a = env.new_workdir("session_a")
    filename = "shared_file.bin"
    content = os.urandom(1024 * 1024)
    with open(os.path.join(cwd_a, filename), "wb") as f:
        f.write(content)
    
    proc_a, input_a, log_a = env.spawn_auth_client(
        cwd_a, username, password, f"UPLOAD {filename} {filename}", "session_a"
    )
    stdout_a, code_a = communicate_and_log(proc_a, input_a, log_a)
    if not has_ok_response(stdout_a):
        results.fail("Auth Multi-Session", "Session A upload failed")
        return

    # Client B downloads immediately
    cwd_b = env.new_workdir("session_b")
    proc_b, input_b, log_b = env.spawn_auth_client(
        cwd_b, username, password, f"DOWNLOAD {filename} downloaded.bin", "session_b"
    )
    stdout_b, code_b = communicate_and_log(proc_b, input_b, log_b)
    
    if not has_ok_response(stdout_b):
        results.fail("Auth Multi-Session", "Session B download failed")
        return
        
    if not os.path.exists(os.path.join(cwd_b, "downloaded.bin")):
        results.fail("Auth Multi-Session", "Downloaded file missing")
        return
        
    if calculate_hash(os.path.join(cwd_b, "downloaded.bin")) != calculate_hash(os.path.join(cwd_a, filename)):
        results.fail("Auth Multi-Session", "Content mismatch")
        return

    results.ok("Authenticated multi-session consistency verified")


def test_auth_race_condition_same_file(env: MultiSessionEnv, results: TestResult):
    """Same user uploading the same file from two sessions concurrently."""
    username = "race_user"
    password = "password"
    env.register_user(username, password, "race_reg")
    
    filename = "race.bin"
    content_a = b"A" * (1024 * 1024)
    content_b = b"B" * (1024 * 1024)
    
    cwd_a = env.new_workdir("race_a")
    with open(os.path.join(cwd_a, filename), "wb") as f:
        f.write(content_a)
        
    cwd_b = env.new_workdir("race_b")
    with open(os.path.join(cwd_b, filename), "wb") as f:
        f.write(content_b)
        
    # Spawn both
    proc_a, input_a, log_a = env.spawn_auth_client(cwd_a, username, password, f"UPLOAD {filename} {filename}", "race_a")
    proc_b, input_b, log_b = env.spawn_auth_client(cwd_b, username, password, f"UPLOAD {filename} {filename}", "race_b")
    
    jobs = [
        lambda: communicate_and_log(proc_a, input_a, log_a),
        lambda: communicate_and_log(proc_b, input_b, log_b)
    ]
    
    _run_parallel_jobs(jobs)
    
    # Verify server is alive
    if not env.is_server_alive():
        results.fail("Auth Race Condition", "Server crashed")
        return
        
    # Verify file content matches A or B
    cwd_check = env.new_workdir("race_check")
    proc_c, input_c, log_c = env.spawn_auth_client(cwd_check, username, password, f"DOWNLOAD {filename} check.bin", "race_check")
    communicate_and_log(proc_c, input_c, log_c)
    
    check_path = os.path.join(cwd_check, "check.bin")
    if not os.path.exists(check_path):
        results.fail("Auth Race Condition", "File missing after race")
        return
        
    with open(check_path, "rb") as f:
        content = f.read()
        
    if content == content_a:
        results.ok("Auth race condition handled (Content A persisted)")
    elif content == content_b:
        results.ok("Auth race condition handled (Content B persisted)")
    else:
        results.fail("Auth Race Condition", "File corrupted (mixed content)")


def test_registration_race_condition(env: MultiSessionEnv, results: TestResult):
    """Concurrent registration of the same user."""
    username = "conflict_user"
    password = "password"
    
    # 1. Connect all clients and try to reach prompt
    # We accept that some might fail to reach the prompt if the server locks early.
    pending = []
    for i in range(4):
        # Use a short timeout so we don't wait too long if blocked
        proc, captured, log_path = env.connect_and_wait_for_register_prompt(username, f"reg_race_{i}", timeout=2)
        if proc is not None:
            pending.append((proc, captured, log_path))
        else:
            # This is expected if server serializes access before prompt
            pass
            
    if not pending:
        results.fail("Registration Race", "No client reached the registration prompt")
        return

    results.ok(f"{len(pending)} clients reached registration prompt")

    # 2. Complete registrations concurrently for those who got the prompt
    jobs = []
    for proc, captured, log_path in pending:
        def _make_job(p=proc, cap=captured, lp=log_path):
            def _job():
                return env.complete_registration(p, password, cap, lp)
            return _job
        jobs.append(_make_job())
        
    run_results = _run_parallel_jobs(jobs)
    
    # 3. Verify results
    # We expect at least one success among the completed ones.
    # Others might fail with "User already exists" or similar.
    success_count = 0
    for stdout, code in run_results:
        # Check for success indicators
        if code == 0:
            success_count += 1
            
    if success_count == 0:
        results.fail("Registration Race", "All clients failed to complete registration")
        return

    # 4. Verify we can actually login
    if verify_registration_login(env, username, password, "verify_race_final"):
        results.ok(f"Registration race handled ({success_count} reported success, login verified)")
    else:
        results.fail("Registration Race", "Login failed after registration")


def main():
    print("MiniDrive Integration Tests - Multiple Sessions")
    print("=" * 60)

    check_executables()

    env = MultiSessionEnv("multi_session", SERVER_PORT)
    results = TestResult(env.log_dir)

    try:
        env.setup_server_root()
        env.start_server()

        test_concurrent_downloads_public(env, results)
        test_concurrent_uploads_public(env, results)
        test_mixed_upload_download_public(env, results)
        test_race_condition_upload(env, results)
        test_auth_multi_session_consistency(env, results)
        test_auth_race_condition_same_file(env, results)
        test_registration_race_condition(env, results)

    finally:
        env.cleanup()

    ok = results.summary()
    print(f"\nLogs saved to: {env.log_dir}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
