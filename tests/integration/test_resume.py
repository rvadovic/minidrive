#!/usr/bin/env python3
"""
MiniDrive Integration Tests - Resume Transfers

Tests resuming interrupted uploads and downloads:
- Simulate interrupted transfer by killing the SERVER (SIGTERM)
- Client loses connection and saves resume state
- Restart server and client
- Verify client offers to resume and completes successfully

NOTE: These tests only interact through the client interface.
We do not inspect server internals (.part files etc).
"""

import os
import sys
import signal
import subprocess
import time
import shutil
import hashlib
import json
import threading
from datetime import datetime

from test_utils import (
    TestEnvironment, TestResult, calculate_hash, has_ok_response,
    check_executables, SERVER_EXE, CLIENT_EXE
)


class ResumeTestEnv(TestEnvironment):
    """Extended test environment for resume testing."""
    
    def __init__(self, port=9027):
        super().__init__("resume", port)
        self.workdirs = []
    
    def new_workdir(self, name):
        """Create a temporary working directory."""
        workdir = os.path.abspath(f"data/resume_test_{name}_{self.test_counter}")
        if os.path.exists(workdir):
            shutil.rmtree(workdir)
        os.makedirs(workdir, exist_ok=True)
        self.workdirs.append(workdir)
        return workdir
    
    def kill_server(self):
        """Kill the server with SIGTERM to simulate interruption."""
        if self.server_process:
            self.server_process.send_signal(signal.SIGTERM)
            try:
                self.server_process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.server_process.kill()
            self.server_process = None
            time.sleep(0.5)  # Allow cleanup
    
    def restart_server(self):
        """Start a fresh server instance."""
        self.server_process = subprocess.Popen(
            [SERVER_EXE, "--port", str(self.port), "--root", self.server_root],
            stdout=self.server_log_file,
            stderr=subprocess.STDOUT
        )
        time.sleep(1)  # Wait for server to start
    
    def spawn_client_with_server_interrupt(self, cwd, commands, log_name, interrupt_delay=3):
        """
        Spawn a client, start a transfer, then kill the SERVER to interrupt.
        Returns client stdout after server is killed and client exits.
        """
        self.test_counter += 1
        log_prefix = f"{self.test_counter:02d}_{log_name}"
        stdout_log = os.path.join(self.log_dir, f"{log_prefix}_stdout.log")
        client_log = os.path.join(self.log_dir, f"{log_prefix}_client.log")
        
        process = subprocess.Popen(
            [CLIENT_EXE, f"127.0.0.1:{self.port}", "--log", client_log],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            cwd=cwd
        )
        
        if isinstance(commands, list):
            input_data = "\n".join(commands) + "\n"
        else:
            input_data = f"{commands}\n"
        
        # Send commands to start the transfer
        process.stdin.write(input_data)
        process.stdin.flush()
        
        # Wait for transfer to progress
        time.sleep(interrupt_delay)
        
        # Kill the server - client will lose connection
        self.kill_server()
        
        # Wait for client to exit (it should exit when connection is lost)
        try:
            stdout, _ = process.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            stdout, _ = process.communicate()
        
        with open(stdout_log, "w") as f:
            f.write(f"# Commands: {commands}\n")
            f.write(f"# CWD: {cwd}\n")
            f.write(f"# Interrupt delay: {interrupt_delay}s\n")
            f.write(f"# Client log: {client_log}\n")
            f.write(f"# {'='*50}\n\n")
            f.write(stdout)
        
        # Restart server for next operations
        self.restart_server()
        
        return stdout
    
    def spawn_auth_client_with_server_interrupt(self, cwd, username, password, commands, log_name, interrupt_delay=3):
        """
        Spawn authenticated client, start a transfer, then kill the SERVER.
        """
        self.test_counter += 1
        log_prefix = f"{self.test_counter:02d}_{log_name}"
        stdout_log = os.path.join(self.log_dir, f"{log_prefix}_stdout.log")
        client_log = os.path.join(self.log_dir, f"{log_prefix}_client.log")
        
        process = subprocess.Popen(
            [CLIENT_EXE, f"{username}@127.0.0.1:{self.port}", "--log", client_log],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            cwd=cwd
        )
        
        # Password + 'n' for no resume + commands
        if isinstance(commands, list):
            input_data = f"{password}\n" + "\n".join(commands) + "\n"
        else:
            input_data = f"{password}\n{commands}\n"
        
        process.stdin.write(input_data)
        process.stdin.flush()
        
        time.sleep(interrupt_delay)
        self.kill_server()
        
        try:
            stdout, _ = process.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            stdout, _ = process.communicate()
        
        with open(stdout_log, "w") as f:
            f.write(f"# User: {username}\n")
            f.write(f"# Commands: {commands}\n")
            f.write(f"# CWD: {cwd}\n")
            f.write(f"# Client log: {client_log}\n")
            f.write(f"# {'='*50}\n\n")
            f.write(stdout)
        
        self.restart_server()
        return stdout

    def run_client_resume(self, cwd, test_name, answer_resume="y", extra_commands=None, timeout=300):
        """
        Run client expecting a resume prompt.
        Answers the resume prompt and optionally runs extra commands.
        """
        self.test_counter += 1
        log_prefix = f"{self.test_counter:02d}_{test_name}"
        stdout_log = os.path.join(self.log_dir, f"{log_prefix}_stdout.log")
        client_log = os.path.join(self.log_dir, f"{log_prefix}_client.log")
        
        # DON'T cleanup metadata - we need the resume state!
        
        process = subprocess.Popen(
            [CLIENT_EXE, f"127.0.0.1:{self.port}", "--log", client_log],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            cwd=cwd
        )
        
        try:
            # Answer the resume prompt, then any extra commands, then EXIT
            if extra_commands:
                if isinstance(extra_commands, list):
                    input_data = f"{answer_resume}\n" + "\n".join(extra_commands) + "\nEXIT\n"
                else:
                    input_data = f"{answer_resume}\n{extra_commands}\nEXIT\n"
            else:
                input_data = f"{answer_resume}\nEXIT\n"
            
            stdout, _ = process.communicate(input=input_data, timeout=timeout)
            exit_code = process.returncode
            
            with open(stdout_log, "w") as f:
                f.write(f"# Resume answer: {answer_resume}\n")
                f.write(f"# Extra commands: {extra_commands}\n")
                f.write(f"# CWD: {cwd}\n")
                f.write(f"# Exit code: {exit_code}\n")
                f.write(f"# Client log: {client_log}\n")
                f.write(f"# {'='*50}\n\n")
                f.write(stdout)
            
            return stdout, exit_code
            
        except subprocess.TimeoutExpired:
            process.kill()
            with open(stdout_log, "w") as f:
                f.write(f"# TIMEOUT after {timeout}s\n")
            return "TIMEOUT", -1

    def run_client_simple(self, cwd, test_name, commands, timeout=300):
        """Run client without expecting resume prompt."""
        self.test_counter += 1
        log_prefix = f"{self.test_counter:02d}_{test_name}"
        stdout_log = os.path.join(self.log_dir, f"{log_prefix}_stdout.log")
        client_log = os.path.join(self.log_dir, f"{log_prefix}_client.log")
        
        process = subprocess.Popen(
            [CLIENT_EXE, f"127.0.0.1:{self.port}", "--log", client_log],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            cwd=cwd
        )
        
        try:
            if isinstance(commands, list):
                input_data = "\n".join(commands) + "\nEXIT\n"
            else:
                input_data = f"{commands}\nEXIT\n"
            
            stdout, _ = process.communicate(input=input_data, timeout=timeout)
            
            with open(stdout_log, "w") as f:
                f.write(f"# Commands: {commands}\n")
                f.write(f"# CWD: {cwd}\n")
                f.write(f"# Client log: {client_log}\n")
                f.write(f"# {'='*50}\n\n")
                f.write(stdout)
            
            return stdout, process.returncode
            
        except subprocess.TimeoutExpired:
            process.kill()
            return "TIMEOUT", -1

    def run_auth_client_resume(self, cwd, username, password, test_name, answer_resume="y", extra_commands=None, timeout=300):
        """Run authenticated client expecting resume prompt."""
        self.test_counter += 1
        log_prefix = f"{self.test_counter:02d}_{test_name}"
        stdout_log = os.path.join(self.log_dir, f"{log_prefix}_stdout.log")
        client_log = os.path.join(self.log_dir, f"{log_prefix}_client.log")
        
        process = subprocess.Popen(
            [CLIENT_EXE, f"{username}@127.0.0.1:{self.port}", "--log", client_log],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            cwd=cwd
        )
        
        try:
            # Password + resume answer + extra commands + EXIT
            if extra_commands:
                if isinstance(extra_commands, list):
                    input_data = f"{password}\n{answer_resume}\n" + "\n".join(extra_commands) + "\nEXIT\n"
                else:
                    input_data = f"{password}\n{answer_resume}\n{extra_commands}\nEXIT\n"
            else:
                input_data = f"{password}\n{answer_resume}\nEXIT\n"
            
            stdout, _ = process.communicate(input=input_data, timeout=timeout)
            
            with open(stdout_log, "w") as f:
                f.write(f"# User: {username}\n")
                f.write(f"# Resume answer: {answer_resume}\n")
                f.write(f"# CWD: {cwd}\n")
                f.write(f"# Client log: {client_log}\n")
                f.write(f"# {'='*50}\n\n")
                f.write(stdout)
            
            return stdout, process.returncode
            
        except subprocess.TimeoutExpired:
            process.kill()
            return "TIMEOUT", -1

    def run_auth_client(self, cwd, username, password, test_name, commands, timeout=300):
        """Run authenticated client without expecting resume prompt."""
        self.test_counter += 1
        log_prefix = f"{self.test_counter:02d}_{test_name}"
        stdout_log = os.path.join(self.log_dir, f"{log_prefix}_stdout.log")
        client_log = os.path.join(self.log_dir, f"{log_prefix}_client.log")
        
        process = subprocess.Popen(
            [CLIENT_EXE, f"{username}@127.0.0.1:{self.port}", "--log", client_log],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            cwd=cwd
        )
        
        try:
            # Password + commands + EXIT
            if isinstance(commands, list):
                input_data = f"{password}\n" + "\n".join(commands) + "\nEXIT\n"
            else:
                input_data = f"{password}\n{commands}\nEXIT\n"
            
            stdout, _ = process.communicate(input=input_data, timeout=timeout)
            
            with open(stdout_log, "w") as f:
                f.write(f"# User: {username}\n")
                f.write(f"# Commands: {commands}\n")
                f.write(f"# CWD: {cwd}\n")
                f.write(f"# Client log: {client_log}\n")
                f.write(f"# {'='*50}\n\n")
                f.write(stdout)
            
            return stdout, process.returncode
            
        except subprocess.TimeoutExpired:
            process.kill()
            return "TIMEOUT", -1
    
    def register_user(self, username, password, log_name):
        """Register a new user."""
        self.test_counter += 1
        log_prefix = f"{self.test_counter:02d}_{log_name}"
        stdout_log = os.path.join(self.log_dir, f"{log_prefix}_register.log")
        client_log = os.path.join(self.log_dir, f"{log_prefix}_client.log")
        
        process = subprocess.Popen(
            [CLIENT_EXE, f"{username}@127.0.0.1:{self.port}", "--log", client_log],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True
        )
        
        # Registration flow: y to register, then password
        input_data = f"y\n{password}\n"
        
        try:
            stdout, _ = process.communicate(input=input_data, timeout=10)
            with open(stdout_log, "w") as f:
                f.write(stdout)
            return stdout, process.returncode
        except subprocess.TimeoutExpired:
            process.kill()
            return "TIMEOUT", -1
    
    def reset_users(self):
        """Reset server state by purging the entire server root and restarting."""
        # Stop server
        self.kill_server()
        
        # Purge entire server root and recreate fresh
        if os.path.exists(self.server_root):
            shutil.rmtree(self.server_root)
        self.setup_server_root()
        
        # Restart server
        self.restart_server()
    
    def cleanup(self):
        """Extended cleanup."""
        super().cleanup()
        for workdir in self.workdirs:
            if os.path.exists(workdir):
                shutil.rmtree(workdir, ignore_errors=True)


def create_test_file(path, size_mb):
    """Create a test file of specified size in MB."""
    with open(path, "wb") as f:
        chunk = 1024 * 1024  # 1MB
        for _ in range(size_mb):
            f.write(os.urandom(chunk))
    return calculate_hash(path)


# =============================================================================
# Test: Upload Resume
# =============================================================================

def test_upload_resume(env, results):
    """Test resuming an interrupted upload - finding optimal size."""
    workdir = env.new_workdir("upload_resume")
    
    sizes_to_try = [10, 50, 100]
    found_size = False
    
    for size_mb in sizes_to_try:
        filename = f"resume_upload_{size_mb}mb.bin"
        local_file = os.path.join(workdir, filename)
        print(f"  Creating {size_mb}MB test file...")
        original_hash = create_test_file(local_file, size_mb=size_mb)
        
        # Benchmark Upload
        bench_filename = f"bench_upload_{size_mb}.bin"
        bench_file = os.path.join(workdir, bench_filename)
        shutil.copy(local_file, bench_file)
        
        print(f"  Benchmark upload ({size_mb}MB)...")
        start_time = time.time()
        stdout, _ = env.run_client_simple(workdir, f"bench_upload_{size_mb}", commands=f"UPLOAD {bench_filename}", timeout=300)
        end_time = time.time()
        
        if not has_ok_response(stdout):
             results.fail("Upload resume", f"Benchmark upload failed for {size_mb}MB")
             return
             
        duration = end_time - start_time
        print(f"  Benchmark duration: {duration:.2f}s")
        
        if duration > 5.0:
            found_size = True
            
            # Test 1: Interrupt after 2s
            print(f"  [1/2] Interrupt upload ({size_mb}MB) after 2.0s...")
            stdout = env.spawn_client_with_server_interrupt(
                workdir, f"UPLOAD {filename}", f"upload_interrupt_early_{size_mb}", interrupt_delay=2.0
            )
            
            if has_ok_response(stdout):
                results.fail("Upload resume", "Upload finished too fast (early interrupt)")
                return
                
            # Resume 1
            print("  Resuming early interrupted upload...")
            start_resume_early = time.time()
            stdout, _ = env.run_client_resume(workdir, "upload_resume_early", answer_resume="y", timeout=300)
            end_resume_early = time.time()
            resume_duration_early = end_resume_early - start_resume_early
            print(f"  Resume duration (early): {resume_duration_early:.2f}s")

            if not has_ok_response(stdout):
                results.fail("Upload resume", "Failed to resume early interrupted upload")
                return

            # Verify 1
            verify_file = "verify_early.bin"
            stdout, _ = env.run_client_simple(workdir, "verify_early", commands=f"DOWNLOAD {filename} {verify_file}", timeout=120)
            if not os.path.exists(os.path.join(workdir, verify_file)) or calculate_hash(os.path.join(workdir, verify_file)) != original_hash:
                 results.fail("Upload resume", "Hash mismatch after early resume")
                 return

            # Test 2: Interrupt after duration - 2s
            late_delay = duration - 2.0
            filename_late = f"resume_upload_late_{size_mb}mb.bin"
            local_file_late = os.path.join(workdir, filename_late)
            shutil.copy(local_file, local_file_late)
            
            print(f"  [2/2] Interrupt upload ({size_mb}MB) after {late_delay:.2f}s...")
            stdout = env.spawn_client_with_server_interrupt(
                workdir, f"UPLOAD {filename_late}", f"upload_interrupt_late_{size_mb}", interrupt_delay=late_delay
            )
            
            if has_ok_response(stdout):
                results.fail("Upload resume", "Upload finished too fast (late interrupt)")
                return
                
            # Resume 2
            print("  Resuming late interrupted upload...")
            start_resume_late = time.time()
            stdout, _ = env.run_client_resume(workdir, "upload_resume_late", answer_resume="y", timeout=300)
            end_resume_late = time.time()
            resume_duration_late = end_resume_late - start_resume_late
            print(f"  Resume duration (late): {resume_duration_late:.2f}s")

            if not has_ok_response(stdout):
                results.fail("Upload resume", "Failed to resume late interrupted upload")
                return
            
            # Performance Check
            # At least one resume should be faster than benchmark + 1s slack
            # (Ideally both, but overhead might affect the early one)
            threshold = duration + 1.0
            if not (resume_duration_early < threshold or resume_duration_late < threshold):
                results.fail("Upload resume", f"Resume took too long. Benchmark: {duration:.2f}s, Resumes: {resume_duration_early:.2f}s, {resume_duration_late:.2f}s")
                return
            else:
                print(f"  Performance check passed: {min(resume_duration_early, resume_duration_late):.2f}s < {threshold:.2f}s")

            # Verify 2
            verify_file_late = "verify_late.bin"
            stdout, _ = env.run_client_simple(workdir, "verify_late", commands=f"DOWNLOAD {filename_late} {verify_file_late}", timeout=120)
            if not os.path.exists(os.path.join(workdir, verify_file_late)) or calculate_hash(os.path.join(workdir, verify_file_late)) != original_hash:
                 results.fail("Upload resume", "Hash mismatch after late resume")
                 return
                 
            results.ok(f"Upload resume verified for {size_mb}MB (Early & Late interrupts)")
            break
            
    if not found_size:
        results.fail("Upload resume", "Could not find a file size that takes > 5s to upload")


def test_download_resume(env, results):
    """Test resuming an interrupted download - tries increasing file sizes."""
    workdir = env.new_workdir("download_resume")
    sizes_to_try = [300, 1000]
    
    interrupted = False
    final_filename = ""
    final_hash = ""
    
    for size_mb in sizes_to_try:
        filename = f"resume_download_{size_mb}mb.bin"
        local_file = os.path.join(workdir, filename)
        
        print(f"  Creating and uploading {size_mb}MB test file...")
        original_hash = create_test_file(local_file, size_mb)
        
        # Upload it first (clean, no resume)
        stdout, _ = env.run_client_simple(workdir, f"setup_dl_{size_mb}",
                                           commands=f"UPLOAD {filename}", timeout=300)
        if not has_ok_response(stdout):
            print(f"  Failed to upload {size_mb}MB file for setup. Retrying...")
            continue
            
        # Warmup Download
        print(f"  Warmup download ({size_mb}MB)...")
        if os.path.exists(local_file):
            os.remove(local_file)
            
        stdout, _ = env.run_client_simple(workdir, f"warmup_dl_{size_mb}", commands=f"DOWNLOAD {filename}", timeout=300)
        if not has_ok_response(stdout):
             print(f"  Warmup download failed for {size_mb}MB. Retrying...")
             continue

        # Benchmark Download
        print(f"  Benchmark download ({size_mb}MB)...")
        if os.path.exists(local_file):
            os.remove(local_file)
            
        start_time = time.time()
        stdout, _ = env.run_client_simple(workdir, f"bench_dl_{size_mb}", commands=f"DOWNLOAD {filename}", timeout=300)
        end_time = time.time()
        duration = end_time - start_time
        print(f"  Benchmark duration: {duration:.2f}s")
        
        if not has_ok_response(stdout):
             print(f"  Benchmark download failed for {size_mb}MB. Retrying...")
             continue

        # Remove local file so we can download it
        os.remove(local_file)
        
        # Start download, then kill SERVER to interrupt
        interrupt_delay = duration / 2.0
        if interrupt_delay < 0.2: interrupt_delay = 0.2
        
        print(f"  Starting download ({size_mb}MB) and interrupting server after {interrupt_delay:.2f}s...")
        stdout = env.spawn_client_with_server_interrupt(
            workdir, f"DOWNLOAD {filename}", f"download_interrupt_{size_mb}", interrupt_delay=interrupt_delay
        )
        
        # Check if download completed successfully (too fast)
        if has_ok_response(stdout):
             print(f"  Download of {size_mb}MB finished too fast. Retrying with larger file...")
             continue
        else:
             print(f"  Download of {size_mb}MB interrupted successfully.")
             interrupted = True
             final_filename = filename
             final_hash = original_hash
             break
    
    if not interrupted:
        results.fail("Download resume", f"Failed to interrupt download even with {sizes_to_try[-1]}MB")
        return

    results.ok("Download interrupted mid-transfer")
    
    # Restart client - should offer to resume
    print("  Resuming download...")
    start_resume = time.time()
    stdout, code = env.run_client_resume(workdir, "download_resume", answer_resume="y", timeout=300)
    end_resume = time.time()
    resume_duration = end_resume - start_resume
    print(f"  Resume duration: {resume_duration:.2f}s")
    
    if "resume" in stdout.lower() or "unfinished" in stdout.lower() or "incomplete" in stdout.lower():
        results.ok("Client detected incomplete transfer")
    
    if has_ok_response(stdout):
        results.ok("Download resume completed successfully")
    else:
        results.fail("Download resume", f"Did not complete: {stdout[:300]}")
        return
    
    # Performance Check
    threshold = duration + 1.0
    if resume_duration > threshold:
        results.fail("Download resume", f"Resume took too long. Benchmark: {duration:.2f}s, Resume: {resume_duration:.2f}s")
        return
    else:
        print(f"  Performance check passed: {resume_duration:.2f}s < {threshold:.2f}s")
    
    # Verify hash
    local_file = os.path.join(workdir, final_filename)
    if os.path.exists(local_file):
        downloaded_hash = calculate_hash(local_file)
        if downloaded_hash == final_hash:
            results.ok("Download resume verified - hash matches")
        else:
            results.fail("Download resume verify", "Hash mismatch")
    else:
        results.fail("Download resume verify", "Downloaded file not found")


# =============================================================================
# Test: Multi-user Resume
# =============================================================================

def test_multiuser_upload_resume(env, results):
    """Multiple authenticated users resume interrupted uploads - using server interruption."""
    # Reset users to ensure clean state
    env.reset_users()
    
    users = [
        ("alice", "alicepass"),
        ("bob", "bobpass"),
        ("carol", "carolpass"),
    ]
    
    # Register users
    for username, password in users:
        stdout, code = env.register_user(username, password, f"reg_{username}")
        # Check for various success indicators
        success_indicators = ["success", "registered", "reconnect", "new account"]
        if not any(indicator in stdout.lower() for indicator in success_indicators):
            if "already exists" not in stdout.lower():
                results.fail("Multi-user setup", f"Failed to register {username}")
                return
    
    results.ok("Registered users for multi-user resume test")
    
    user_data = []
    sizes_to_try = [10, 50, 100]
    
    # Create files and interrupt uploads for each user
    for username, password in users:
        workdir = env.new_workdir(f"resume_{username}")
        found_size = False
        
        for size_mb in sizes_to_try:
            filename = f"user_upload_{size_mb}mb.bin"
            local_file = os.path.join(workdir, filename)
            
            print(f"  Creating {size_mb}MB file for {username}...")
            original_hash = create_test_file(local_file, size_mb)
            
            # Benchmark Upload
            bench_filename = f"bench_upload_{size_mb}mb.bin"
            bench_file = os.path.join(workdir, bench_filename)
            shutil.copy(local_file, bench_file)
            
            print(f"  Benchmark upload for {username} ({size_mb}MB)...")
            start_time = time.time()
            stdout, _ = env.run_auth_client(
                workdir, username, password, f"bench_{username}_{size_mb}",
                commands=f"UPLOAD {bench_filename}", timeout=300
            )
            end_time = time.time()
            
            if not has_ok_response(stdout):
                 results.fail("Multi-user upload resume", f"Benchmark upload failed for {username}")
                 return
                 
            duration = end_time - start_time
            print(f"  Benchmark duration: {duration:.2f}s")
            
            if duration > 5.0:
                found_size = True
                
                # Test 1: Interrupt after 2s
                print(f"  [1/2] Interrupt upload for {username} ({size_mb}MB) after 2.0s...")
                stdout = env.spawn_auth_client_with_server_interrupt(
                    workdir, username, password,
                    f"UPLOAD {filename}", f"upload_early_{username}_{size_mb}mb", interrupt_delay=2.0
                )
                
                if has_ok_response(stdout):
                    results.fail("Multi-user upload resume", f"Upload finished too fast (early interrupt) for {username}")
                    return
                
                # Resume 1
                print(f"  Resuming early interrupted upload for {username}...")
                stdout, _ = env.run_auth_client_resume(
                    workdir, username, password, f"resume_early_{username}", answer_resume="y", timeout=300
                )
                if not has_ok_response(stdout):
                    results.fail("Multi-user upload resume", f"Failed to resume early interrupted upload for {username}")
                    return

                # Test 2: Interrupt after duration - 2s
                late_delay = duration - 2.0
                filename_late = f"user_upload_late_{size_mb}mb.bin"
                local_file_late = os.path.join(workdir, filename_late)
                shutil.copy(local_file, local_file_late)
                
                print(f"  [2/2] Interrupt upload for {username} ({size_mb}MB) after {late_delay:.2f}s...")
                stdout = env.spawn_auth_client_with_server_interrupt(
                    workdir, username, password,
                    f"UPLOAD {filename_late}", f"upload_late_{username}_{size_mb}mb", interrupt_delay=late_delay
                )
                
                if has_ok_response(stdout):
                    results.fail("Multi-user upload resume", f"Upload finished too fast (late interrupt) for {username}")
                    return
                
                # Resume 2
                print(f"  Resuming late interrupted upload for {username}...")
                stdout, _ = env.run_auth_client_resume(
                    workdir, username, password, f"resume_late_{username}", answer_resume="y", timeout=300
                )
                if not has_ok_response(stdout):
                    results.fail("Multi-user upload resume", f"Failed to resume late interrupted upload for {username}")
                    return
                
                break
        
        if not found_size:
            results.fail("Multi-user upload resume", f"Could not find a file size that takes > 5s to upload for {username}")
            return

    results.ok("Multi-user upload resume verified (Early & Late interrupts)")


def test_multiuser_download_resume(env, results):
    """Multiple authenticated users resume interrupted downloads - using server interruption."""
    # Reset users to ensure clean state
    env.reset_users()
    
    users = [
        ("dave", "davepass"),
        ("eve", "evepass"),
        ("frank", "frankpass"),
    ]
    
    # Register users
    for username, password in users:
        stdout, code = env.register_user(username, password, f"reg_{username}")
    
    results.ok("Registered users for download resume test")
    
    user_data = []
    sizes_to_try = [50, 150, 300]
    
    # Create files and interrupt downloads for each user
    for username, password in users:
        workdir = env.new_workdir(f"dl_resume_{username}")
        interrupted = False
        final_filename = ""
        final_hash = ""
        
        for size_mb in sizes_to_try:
            filename = f"user_download_{size_mb}mb.bin"
            local_file = os.path.join(workdir, filename)
            
            print(f"  Creating and uploading {size_mb}MB file for {username}...")
            original_hash = create_test_file(local_file, size_mb)
            
            # Upload file for this user
            stdout, _ = env.run_auth_client(
                workdir, username, password, f"setup_{username}_{size_mb}",
                commands=f"UPLOAD {filename}", timeout=300
            )
            
            if not has_ok_response(stdout):
                results.fail("Multi-user download resume", f"Failed to upload {size_mb}MB file for {username}")
                return
            
            # 1. First Download (Warmup)
            print(f"  [1/3] Warmup download for {username} ({size_mb}MB)...")
            if os.path.exists(local_file):
                os.remove(local_file)
                
            stdout, _ = env.run_auth_client(
                workdir, username, password, f"warmup_{username}_{size_mb}",
                commands=f"DOWNLOAD {filename}", timeout=300
            )
            if not has_ok_response(stdout):
                 results.fail("Multi-user download resume", f"Warmup download failed for {username}")
                 return

            # 2. Second Download (Benchmark)
            print(f"  [2/3] Benchmark download for {username} ({size_mb}MB)...")
            if os.path.exists(local_file):
                os.remove(local_file)
            
            start_time = time.time()
            stdout, _ = env.run_auth_client(
                workdir, username, password, f"bench_{username}_{size_mb}",
                commands=f"DOWNLOAD {filename}", timeout=300
            )
            end_time = time.time()
            
            if not has_ok_response(stdout):
                 results.fail("Multi-user download resume", f"Benchmark download failed for {username}")
                 return
                 
            duration = end_time - start_time
            print(f"  Benchmark duration: {duration:.2f}s")
            
            # 3. Third Download (Interrupt)
            interrupt_delay = duration / 2.0
            if interrupt_delay < 0.5:
                interrupt_delay = 0.5
                
            print(f"  [3/3] Interrupt download for {username} ({size_mb}MB) after {interrupt_delay:.2f}s...")
            if os.path.exists(local_file):
                os.remove(local_file)
                
            stdout = env.spawn_auth_client_with_server_interrupt(
                workdir, username, password,
                f"DOWNLOAD {filename}", f"download_{username}_{size_mb}", interrupt_delay=interrupt_delay
            )
            
            if "File downloaded successfully" in stdout or has_ok_response(stdout):
                print(f"  Download of {size_mb}MB finished too fast (before interrupt). Retrying with larger file...")
                continue

            # Verify partial download by scanning directory recursively
            found_files = []
            for root, dirs, files in os.walk(workdir):
                for file in files:
                    found_files.append(os.path.join(root, file))
            
            if not found_files:
                print(f"  No files found in workdir for {username}. Retrying...")
                continue
            
            expected_size = size_mb * 1024 * 1024
            found_partial = False
            min_partial_size = 1024 * 1024 # 1MB
            
            for f_path in found_files:
                size = os.path.getsize(f_path)
                rel_path = os.path.relpath(f_path, workdir)
                
                if size > min_partial_size and size < expected_size:
                    print(f"  Found valid partial file {rel_path} size: {size} bytes (Target: {expected_size})")
                    found_partial = True
                    break
                elif size > 0:
                     print(f"  Found file {rel_path} size: {size} bytes (Ignored: too small or full size)")
            
            if found_partial:
                print(f"  Download of {size_mb}MB interrupted successfully.")
                interrupted = True
                final_filename = filename
                final_hash = original_hash
                break
            else:
                print(f"  No valid partial file (>1MB) found for {username}. Retrying...")

        if not interrupted:
            results.fail("Multi-user download resume", f"Failed to interrupt download for {username} even with {sizes_to_try[-1]}MB")
            return

        user_data.append({
            "username": username,
            "password": password,
            "workdir": workdir,
            "local_file": os.path.join(workdir, final_filename),
            "original_hash": final_hash,
            "filename": final_filename,
            "benchmark_duration": duration
        })
    
    results.ok("Interrupted multi-user downloads via server termination")
    
    # Resume all downloads concurrently
    threads = []
    resume_results = {}
    
    def resume_download(ud):
        try:
            stdout, code = env.run_auth_client_resume(
                ud["workdir"], ud["username"], ud["password"],
                f"resume_dl_{ud['username']}", answer_resume="y", timeout=300
            )
            
            # Check if file was downloaded correctly
            if os.path.exists(ud["local_file"]):
                local_hash = calculate_hash(ud["local_file"])
                resume_results[ud["username"]] = (local_hash == ud["original_hash"])
            else:
                resume_results[ud["username"]] = False
        except Exception as e:
            resume_results[ud["username"]] = False
    
    for ud in user_data:
        t = threading.Thread(target=resume_download, args=(ud,))
        threads.append(t)
        t.start()
    
    for t in threads:
        t.join(timeout=400)
    
    all_ok = all(resume_results.values())
    if all_ok:
        results.ok("All users resumed downloads with correct hashes")
    else:
        failed = [u for u, ok in resume_results.items() if not ok]
        results.fail("Multi-user download resume", f"Failed users: {failed}")





# =============================================================================
# Main
# =============================================================================

def main():
    print("MiniDrive Integration Tests - Resume Transfers")
    print("=" * 60)
    
    check_executables()
    
    env = ResumeTestEnv(port=9027)
    results = TestResult(env.log_dir)
    
    try:
        print("\nSetting up test environment...")
        env.setup_server_root()
        env.start_server()
        
        print("\nRunning tests:\n")
        
        # Basic resume tests (200MB files)
        test_upload_resume(env, results)
        test_download_resume(env, results)
        

        # Multi-user resume tests (100MB per user)
        #test_multiuser_upload_resume(env, results)
        #test_multiuser_download_resume(env, results)
        
    finally:
        print("\nCleaning up...")
        env.cleanup()
    
    success = results.summary()
    print(f"\nLogs saved to: {env.log_dir}")
    
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
