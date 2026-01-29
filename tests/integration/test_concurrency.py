#!/usr/bin/env python3
"""Integration tests for MiniDrive concurrency (Distinct Users).

Goal:
- Validate the server can handle multiple DISTINCT authenticated users concurrently.
- Ensure user isolation is maintained under load.

Note:
- Multiple sessions for the SAME user (or public mode) are tested in test_multiple_sessions.py.

Usage:
    python3 tests/integration/test_concurrency.py
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

SERVER_PORT = 9025


class ConcurrencyEnv(TestEnvironment):

    def is_server_alive(self) -> bool:
        return self.server_process is not None and self.server_process.poll() is None

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

    def spawn_public_client(self, cwd: str, commands, label: str):
        """Spawn public client process and return (proc, input_data, stdout_log_path)."""
        if isinstance(commands, list):
            input_lines = commands + ["EXIT"]
        else:
            input_lines = [commands, "EXIT"]

        stdout_log = os.path.join(self.log_dir, f"{label}_stdout.log")
        client_log = os.path.join(self.log_dir, f"{label}_client.log")
        proc = self.start_client_process(f"127.0.0.1:{self.port}", client_log, cwd=cwd)
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


def verify_registration_login(env: ConcurrencyEnv, username: str, password: str, label: str) -> bool:
    """Verify registration by logging in and running LIST."""
    cwd = env.new_workdir(f"{label}_verify")
    # Just run LIST
    proc, input_data, stdout_log = env.spawn_auth_client(cwd, username, password, "LIST", f"{label}_verify")
    stdout, code = communicate_and_log(proc, input_data, stdout_log)
    return has_ok_response(stdout)


def test_concurrent_registrations(env: ConcurrencyEnv, results: TestResult):
    """Multiple distinct users register concurrently."""
    users = [
        ("reg_user1", "pass1"),
        ("reg_user2", "pass2"),
        ("reg_user3", "pass3"),
        ("reg_user4", "pass4"),
    ]
    
    # 1. Connect all clients and wait for prompt
    pending = []
    for username, password in users:
        proc, captured, log_path = env.connect_and_wait_for_register_prompt(username, f"concurrent_reg_{username}")
        if proc is None:
            results.fail("Concurrent registrations", f"Client for {username} failed to reach prompt")
            return
        pending.append((proc, password, captured, log_path))
        
    results.ok("All clients synced at register prompt")
    
    # 2. Complete registrations concurrently
    jobs = []
    for proc, password, captured, log_path in pending:
        def _make_job(p=proc, pw=password, cap=captured, lp=log_path):
            def _job():
                return env.complete_registration(p, pw, cap, lp)
            return _job
        jobs.append(_make_job())
        
    run_results = _run_parallel_jobs(jobs)
    
    for i, (stdout, code) in enumerate(run_results):
        username = users[i][0]
        password = users[i][1]
        
        if code != 0:
            results.fail("Concurrent registrations", f"Registration process failed (exit code {code}) for {username}")
            return
            
        if not verify_registration_login(env, username, password, f"verify_reg_{username}"):
            results.fail("Concurrent registrations", f"Login verification failed for {username}")
            return
            
    results.ok("Concurrent registrations succeeded")


def test_authenticated_multiuser_concurrency(env: ConcurrencyEnv, results: TestResult):
    """Multiple distinct authenticated users upload/download concurrently.

    Validates:
    - Concurrent authenticated sessions work.
    - User private namespaces remain isolated (same remote filename differs per user).
    """

    users = [
        ("alice", "alicepass"),
        ("bob", "bobpass"),
        ("carol", "carolpass"),
        ("dave", "davepass"),
    ]

    # Register users first (registration closes the connection; must reconnect for login).
    for username, password in users:
        stdout, code = env.register_user(username, password, f"auth_register_{username}")
        if code != 0:
            results.fail("Auth multi-user concurrency", f"Registration process failed for {username}")
            return
        if not verify_registration_login(env, username, password, f"verify_auth_{username}"):
            results.fail("Auth multi-user concurrency", f"Login verification failed for {username}")
            return
            
    results.ok("Registered distinct users")

    remote_name = "same_name.bin"
    jobs = []
    expectations = []

    for username, password in users:
        cwd = env.new_workdir(f"auth_{username}")
        local_name = "local.bin"
        download_name = "downloaded.bin"

        local_path = os.path.join(cwd, local_name)
        content = (f"user={username}\n".encode("utf-8") + os.urandom(128 * 1024))
        with open(local_path, "wb") as f:
            f.write(content)

        expected_hash = calculate_hash(local_path)
        # We don't know exact path structure, but we can verify file exists in server root
        # and has correct content.
        expectations.append((username, cwd, download_name, expected_hash, remote_name))

        def _make_job(u=username, p=password, work=cwd):
            def _job():
                cmd_list = [
                    f"UPLOAD {local_name} {remote_name}",
                    f"DOWNLOAD {remote_name} {download_name}",
                ]
                proc, input_data, stdout_log = env.spawn_auth_client(
                    work,
                    u,
                    p,
                    cmd_list,
                    f"auth_{u}",
                )
                stdout, code = communicate_and_log(proc, input_data, stdout_log, timeout=60)
                return u, stdout, code, stdout_log

            return _job

        jobs.append(_make_job())

    results.ok("Spawned concurrent authenticated clients")

    run_results = _run_parallel_jobs(jobs)
    for username, stdout, code, stdout_log in run_results:
        if not has_ok_response(stdout):
            results.fail(
                "Auth multi-user concurrency",
                f"User {username} did not return OK. See {stdout_log}",
            )
            return

    # Validate downloaded files match per-user uploaded content and server stored in correct private dirs.
    for username, cwd, download_name, expected_hash, remote_name in expectations:
        out_path = os.path.join(cwd, download_name)
        if not os.path.exists(out_path):
            results.fail("Auth multi-user concurrency", f"Missing download for {username}: {out_path}")
            return
        if calculate_hash(out_path) != expected_hash:
            results.fail("Auth multi-user concurrency", f"Downloaded content mismatch for {username}")
            return
            
    results.ok("Concurrent authenticated upload/download succeeds with per-user isolation")


def test_mixed_auth_public_concurrency(env: ConcurrencyEnv, results: TestResult):
    """Mix of authenticated users and public clients doing operations."""
    
    # Setup: Register one user
    username = "mixed_user"
    password = "mixed_pass"
    stdout, code = env.register_user(username, password, "mixed_reg")
    if code != 0 or not verify_registration_login(env, username, password, "verify_mixed"):
        results.fail("Mixed concurrency", "Registration failed")
        return

    # Prepare files
    # Auth user uploads "auth.bin"
    # Public user uploads "public.bin"
    
    jobs = []
    
    # Job 1: Auth User Upload
    cwd_auth = env.new_workdir("mixed_auth")
    local_auth = "auth.bin"
    remote_auth = "auth_remote.bin"
    with open(os.path.join(cwd_auth, local_auth), "wb") as f:
        f.write(os.urandom(1024 * 1024)) # 1MB
    
    def _job_auth():
        proc, input_data, stdout_log = env.spawn_auth_client(
            cwd_auth, username, password, f"UPLOAD {local_auth} {remote_auth}", "mixed_auth_up"
        )
        return communicate_and_log(proc, input_data, stdout_log)
    jobs.append(_job_auth)
    
    # Job 2: Public User Upload
    cwd_pub = env.new_workdir("mixed_pub")
    local_pub = "public.bin"
    remote_pub = "public_remote.bin"
    with open(os.path.join(cwd_pub, local_pub), "wb") as f:
        f.write(os.urandom(1024 * 1024)) # 1MB
        
    def _job_pub():
        proc, input_data, stdout_log = env.spawn_public_client(
            cwd_pub, f"UPLOAD {local_pub} {remote_pub}", "mixed_pub_up"
        )
        return communicate_and_log(proc, input_data, stdout_log)
    jobs.append(_job_pub)
    
    # Run concurrently
    run_results = _run_parallel_jobs(jobs)
    
    for stdout, code in run_results:
        if not has_ok_response(stdout):
            results.fail("Mixed concurrency", "Upload failed")
            return
            
    # Verify isolation: Public file should be in public/, Auth file in private/user/
    # We verify by downloading back.
    
    # Job 3: Auth User Download
    def _job_auth_dl():
        proc, input_data, stdout_log = env.spawn_auth_client(
            cwd_auth, username, password, f"DOWNLOAD {remote_auth} dl_auth.bin", "mixed_auth_dl"
        )
        return communicate_and_log(proc, input_data, stdout_log)
        
    # Job 4: Public User Download
    def _job_pub_dl():
        proc, input_data, stdout_log = env.spawn_public_client(
            cwd_pub, f"DOWNLOAD {remote_pub} dl_pub.bin", "mixed_pub_dl"
        )
        return communicate_and_log(proc, input_data, stdout_log)
        
    run_results = _run_parallel_jobs([_job_auth_dl, _job_pub_dl])
    
    for stdout, code in run_results:
        if not has_ok_response(stdout):
            results.fail("Mixed concurrency", "Download failed")
            return
            
    if not os.path.exists(os.path.join(cwd_auth, "dl_auth.bin")):
        results.fail("Mixed concurrency", "Auth download missing")
        return
    if not os.path.exists(os.path.join(cwd_pub, "dl_pub.bin")):
        results.fail("Mixed concurrency", "Public download missing")
        return
        
    results.ok("Mixed Auth/Public concurrency works")


def test_large_upload_concurrency(env: ConcurrencyEnv, results: TestResult):
    """Test large file upload concurrent with small file upload.
    
    Scenario:
    1. Public user starts uploading 1GB file.
    2. Auth user logs in and uploads small file.
    3. Auth user should finish successfully.
    4. Public user is terminated (simulating cancel/timeout).
    """
    
    # Setup: Register user
    username = "fast_user"
    password = "fast_pass"
    stdout, code = env.register_user(username, password, "fast_reg")
    if code != 0 or not verify_registration_login(env, username, password, "verify_fast"):
        results.fail("Large upload concurrency", "Registration failed")
        return

    # Prepare 1GB file (sparse/zeros)
    cwd_large = env.new_workdir("large_upload")
    large_file = "large_1gb.bin"
    large_path = os.path.join(cwd_large, large_file)
    
    # Create 1GB file efficiently
    with open(large_path, "wb") as f:
        f.seek(1024 * 1024 * 1000 - 1) # 1000MB
        f.write(b"\0")
        
    # Prepare small file
    cwd_small = env.new_workdir("small_upload")
    small_file = "small.bin"
    small_path = os.path.join(cwd_small, small_file)
    with open(small_path, "wb") as f:
        f.write(b"small content" * 100)
        
    # Start Large Upload (Public)
    # We use spawn_public_client but don't wait for it yet
    proc_large, input_large, log_large = env.spawn_public_client(
        cwd_large, f"UPLOAD {large_file} remote_large.bin", "large_client"
    )
    
    # Send input but don't wait for finish yet. 
    # Actually spawn_public_client returns (proc, input_str, log_path).
    # We need to feed input to stdin.
    
    # We'll use a thread to feed input to large client so we don't block
    def _feed_large():
        try:
            proc_large.communicate(input=input_large, timeout=120)
        except:
            pass # Expected to be killed or timeout
            
    thread_large = threading.Thread(target=_feed_large)
    thread_large.start()
    
    # Give it a moment to establish connection and start transferring
    time.sleep(3.0)
    
    # Start Small Upload (Auth)
    proc_small, input_small, log_small = env.spawn_auth_client(
        cwd_small, username, password, f"UPLOAD {small_file} remote_small.bin", "small_client"
    )
    
    # This should finish quickly
    stdout_small, code_small = communicate_and_log(proc_small, input_small, log_small, timeout=15)
    
    if not has_ok_response(stdout_small):
        results.fail("Large upload concurrency", "Small upload failed while large upload was active")
        # Cleanup
        proc_large.kill()
        thread_large.join()
        return
        
    # Verify small file exists on server (by downloading it back)
    cwd_verify = env.new_workdir("verify_small")
    proc_verify, input_verify, log_verify = env.spawn_auth_client(
        cwd_verify, username, password, f"DOWNLOAD remote_small.bin dl_small.bin", "verify_small"
    )
    stdout_verify, code_verify = communicate_and_log(proc_verify, input_verify, log_verify)
    
    if not has_ok_response(stdout_verify):
        results.fail("Large upload concurrency", "Could not verify small file upload")
    else:
        results.ok("Small upload succeeded during large upload")
        
    # Cleanup large upload
    proc_large.kill()
    thread_large.join()


def test_cross_user_interference(env: ConcurrencyEnv, results: TestResult):
    """Ensure one user cannot access another's files under load."""
    admin = "admin_user"
    hacker = "hacker_user"
    password = "password"
    
    env.register_user(admin, password, "reg_admin")
    env.register_user(hacker, password, "reg_hacker")
    
    # Admin uploads secret
    cwd_admin = env.new_workdir("admin")
    secret_file = "secret.txt"
    with open(os.path.join(cwd_admin, secret_file), "wb") as f:
        f.write(b"TOP SECRET DATA")
    
    proc, input_d, log = env.spawn_auth_client(cwd_admin, admin, password, f"UPLOAD {secret_file} {secret_file}", "admin_up")
    communicate_and_log(proc, input_d, log)
    
    # Hacker tries to download secret.txt while Admin is uploading noise
    cwd_hacker = env.new_workdir("hacker")
    
    # Admin noise upload
    noise_file = "noise.bin"
    with open(os.path.join(cwd_admin, noise_file), "wb") as f:
        f.write(os.urandom(1024 * 1024))
        
    def _job_admin():
        proc, inp, log = env.spawn_auth_client(cwd_admin, admin, password, f"UPLOAD {noise_file} {noise_file}", "admin_noise")
        return communicate_and_log(proc, inp, log)
        
    def _job_hacker():
        # Hacker tries to download "secret.txt" - assuming flat namespace or guessing path?
        # In this system, DOWNLOAD filename looks in the user's private folder.
        # So hacker downloading "secret.txt" looks in /private/hacker/secret.txt.
        # It should NOT find /private/admin/secret.txt.
        proc, inp, log = env.spawn_auth_client(cwd_hacker, hacker, password, f"DOWNLOAD {secret_file} stolen.txt", "hacker_steal")
        return communicate_and_log(proc, inp, log)
        
    results_list = _run_parallel_jobs([_job_admin, _job_hacker])
    
    hacker_stdout = results_list[1][0]
    
    if has_ok_response(hacker_stdout):
        results.fail("Cross-User Interference", "Hacker successfully downloaded a file (should be missing)")
    elif "not found" in hacker_stdout.lower() or "error" in hacker_stdout.lower():
        results.ok("Cross-user isolation verified (Hacker got error)")
    else:
        # It might just fail with generic error, which is also good
        results.ok("Cross-user isolation verified (Hacker failed)")


def test_concurrent_mixed_load(env: ConcurrencyEnv, results: TestResult):
    """
    Test mixed load: 2 users downloading (previously uploaded files) 
    and 2 users uploading new files simultaneously.
    Uses 10MB files.
    """
    users_dl = [("dl_user1", "pass1"), ("dl_user2", "pass2")]
    users_up = [("up_user1", "pass3"), ("up_user2", "pass4")]
    all_users = users_dl + users_up
    
    # Register all users
    for username, password in all_users:
        stdout, code = env.register_user(username, password, f"reg_{username}")
        if code != 0:
            results.fail("Mixed Load (10MB)", f"Registration failed for {username}")
            return

    # Prepare 10MB files
    file_size = 10 * 1024 * 1024
    
    # Helper to create file
    def create_10mb_file(path):
        with open(path, "wb") as f:
            # Write some random data at start and end to ensure integrity check works
            f.write(os.urandom(1024))
            # Fill middle with pattern
            chunk = b"x" * 1024 * 1024 # 1MB chunk
            for _ in range(9):
                f.write(chunk)
            # Remaining bytes
            f.write(os.urandom(file_size - f.tell()))

    # Setup workdirs and files
    user_contexts = {} # username -> (cwd, local_file, remote_file, hash)
    
    for username, _ in all_users:
        cwd = env.new_workdir(f"mixed_{username}")
        filename = f"data_{username}.bin"
        local_path = os.path.join(cwd, filename)
        create_10mb_file(local_path)
        file_hash = calculate_hash(local_path)
        user_contexts[username] = {
            "cwd": cwd,
            "file": filename,
            "hash": file_hash
        }

    # Pre-upload for DL users
    for username, password in users_dl:
        ctx = user_contexts[username]
        proc, inp, log = env.spawn_auth_client(
            ctx["cwd"], username, password, 
            f"UPLOAD {ctx['file']} {ctx['file']}", 
            f"pre_upload_{username}"
        )
        stdout, code = communicate_and_log(proc, inp, log, timeout=60)
        if not has_ok_response(stdout):
            results.fail("Mixed Load (10MB)", f"Pre-upload failed for {username}")
            return

    # Concurrent Operations
    jobs = []
    
    # DL Jobs
    for username, password in users_dl:
        ctx = user_contexts[username]
        def _make_dl_job(u=username, p=password, c=ctx):
            def _job():
                proc, inp, log = env.spawn_auth_client(
                    c["cwd"], u, p, 
                    f"DOWNLOAD {c['file']} downloaded_{c['file']}", 
                    f"concurrent_dl_{u}"
                )
                return communicate_and_log(proc, inp, log, timeout=60)
            return _job
        jobs.append(_make_dl_job())

    # UP Jobs
    for username, password in users_up:
        ctx = user_contexts[username]
        def _make_up_job(u=username, p=password, c=ctx):
            def _job():
                proc, inp, log = env.spawn_auth_client(
                    c["cwd"], u, p, 
                    f"UPLOAD {c['file']} {c['file']}", 
                    f"concurrent_up_{u}"
                )
                return communicate_and_log(proc, inp, log, timeout=60)
            return _job
        jobs.append(_make_up_job())

    # Run all 4
    run_results = _run_parallel_jobs(jobs)
    
    # Verify results
    for i, (stdout, code) in enumerate(run_results):
        if not has_ok_response(stdout):
            results.fail("Mixed Load (10MB)", f"Concurrent operation failed for job {i}")
            return

    # Verify Downloads
    for username, _ in users_dl:
        ctx = user_contexts[username]
        dl_path = os.path.join(ctx["cwd"], f"downloaded_{ctx['file']}")
        if not os.path.exists(dl_path):
            results.fail("Mixed Load (10MB)", f"Download missing for {username}")
            return
        if calculate_hash(dl_path) != ctx["hash"]:
            results.fail("Mixed Load (10MB)", f"Download hash mismatch for {username}")
            return

    # Verify Uploads (by downloading them back)
    for username, password in users_up:
        ctx = user_contexts[username]
        # Download to a new name
        proc, inp, log = env.spawn_auth_client(
            ctx["cwd"], username, password, 
            f"DOWNLOAD {ctx['file']} verify_{ctx['file']}", 
            f"verify_up_{username}"
        )
        stdout, code = communicate_and_log(proc, inp, log, timeout=60)
        if not has_ok_response(stdout):
             results.fail("Mixed Load (10MB)", f"Verification download failed for {username}")
             return
        
        verify_path = os.path.join(ctx["cwd"], f"verify_{ctx['file']}")
        if calculate_hash(verify_path) != ctx["hash"]:
            results.fail("Mixed Load (10MB)", f"Upload verification hash mismatch for {username}")
            return

    results.ok("Mixed Load (10MB) - 2 DL / 2 UP concurrent succeeded")


def main():
    print("MiniDrive Integration Tests - Concurrency (Distinct Users)")
    print("=" * 60)

    check_executables()

    env = ConcurrencyEnv("concurrency", SERVER_PORT)
    results = TestResult(env.log_dir)

    try:
        env.setup_server_root()
        env.start_server()

        test_concurrent_registrations(env, results)
        test_authenticated_multiuser_concurrency(env, results)
        test_mixed_auth_public_concurrency(env, results)
        test_large_upload_concurrency(env, results)
        test_cross_user_interference(env, results)
        test_concurrent_mixed_load(env, results)

    finally:
        env.cleanup()

    ok = results.summary()
    print(f"\nLogs saved to: {env.log_dir}")
    sys.exit(0 if ok else 1)



if __name__ == "__main__":
    main()
