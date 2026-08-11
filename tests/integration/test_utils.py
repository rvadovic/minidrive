#!/usr/bin/env python3
"""
Shared utilities for MiniDrive integration tests.

Provides:
- Server/client process management
- Log directory creation and management
- Test result tracking with per-test logging
"""

import subprocess
import time
import os
import signal
import sys
import shutil
import hashlib
import re
import atexit
import tempfile
import select
from datetime import datetime
from pathlib import Path


# Configuration
BUILD_DIR = os.path.abspath("build")

def _get_exe_path(name, subfolder):
    """Find executable, checking build/subfolder/name then build/name."""
    path1 = os.path.join(BUILD_DIR, subfolder, name)
    if os.path.exists(path1):
        return path1
    path2 = os.path.join(BUILD_DIR, name)
    if os.path.exists(path2):
        return path2
    return path1

SERVER_EXE = _get_exe_path("server", "server")
CLIENT_EXE = _get_exe_path("client", "client")


def get_test_log_dir(test_suite_name):
    """
    Create a unique log directory for this test run.
    Returns the path and prints it for correlation.
    """
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_dir = os.path.abspath(f"test_logs/{test_suite_name}_{timestamp}")
    os.makedirs(log_dir, exist_ok=True)
    print(f"\n{'='*60}")
    print(f"LOG DIRECTORY: {log_dir}")
    print(f"{'='*60}\n")
    return log_dir


def calculate_hash(filepath):
    """Calculate SHA-256 hash of a file."""
    sha256 = hashlib.sha256()
    with open(filepath, "rb") as f:
        for block in iter(lambda: f.read(8192), b""):
            sha256.update(block)
    return sha256.hexdigest()


def parse_response_header(stdout: str):
    """Parse the first protocol response header from client output.

    The client output may include extra logging (version, debug, etc.). This
    scans for output that looks like either:
      - OK or [ok]
      - ERROR: <code>

    Returns: (status, code)
      - status: "OK", "ERROR", or None
      - code: int for ERROR, else None
    """

    if not stdout:
        return None, None

    stdout_lower = stdout.lower()
    
    # Check for "ok" anywhere in output (case-insensitive)
    if "ok" in stdout_lower:
        return "OK", None
    
    # Check for error pattern: error anything-without-whitespace at least one whitespace and then digits
    # e.g. "error: 123", "[error] 123"
    error_match = re.search(r"error\S*\s+(\d+)", stdout_lower)
    if error_match:
        return "ERROR", int(error_match.group(1))

    return None, None


def has_ok_response(stdout: str) -> bool:
    status, _ = parse_response_header(stdout)
    return status == "OK"


def get_error_code(stdout: str):
    status, code = parse_response_header(stdout)
    if status != "ERROR":
        return None
    return code


def has_error_with_code(stdout: str) -> bool:
    return get_error_code(stdout) is not None


def communicate_and_log(proc: subprocess.Popen, input_data: str, stdout_log: str, timeout: int = 60):
    """Send input to process, wait for finish, and log stdout."""
    try:
        stdout, _ = proc.communicate(input=input_data, timeout=timeout)
        code = proc.returncode
    except subprocess.TimeoutExpired:
        proc.kill()
        stdout, _ = proc.communicate()
        code = -1

    with open(stdout_log, "w") as f:
        f.write(f"# Exit code: {code}\n")
        f.write(f"# {'='*50}\n\n")
        f.write(stdout or "")

    return stdout or "", code


class TestEnvironment:
    """Manages the test environment including server, logs, and cleanup."""
    
    def __init__(self, suite_name, port):
        self.suite_name = suite_name
        self.port = port
        self.log_dir = get_test_log_dir(suite_name)
        self.server_root = os.path.abspath(f"data/test_{suite_name}_root")
        self.server_process = None
        self.server_log_file = None
        self.test_counter = 0
        
        # Setup client working directory
        self.client_cwd = os.path.abspath(f"data/test_{suite_name}_client_cwd")
        if os.path.exists(self.client_cwd):
            shutil.rmtree(self.client_cwd)
        os.makedirs(self.client_cwd, exist_ok=True)
        
        # Clean up client metadata files that may interfere with tests
        self._cleanup_client_metadata()

        # Register cleanup on exit
        atexit.register(self.cleanup)
        # Register signal handlers for graceful shutdown
        signal.signal(signal.SIGTERM, self._signal_handler)
        signal.signal(signal.SIGINT, self._signal_handler)
        
        # Initialize logging support state (checked lazily)
        self.supports_logging = None

    def _check_logging_support(self):
        """Check if the client supports the --log argument."""
        process = None
        try:
            # Try running with --log argument
            # Use a temp file for the log check to avoid cluttering
            with tempfile.NamedTemporaryFile(delete=False) as tmp:
                tmp_log = tmp.name

            process = subprocess.Popen(
                [CLIENT_EXE, f"127.0.0.1:{self.port}", "--log", tmp_log],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                cwd=self.client_cwd
            )
            stdout, _ = process.communicate(input="EXIT\n", timeout=2)

            if os.path.exists(tmp_log):
                os.remove(tmp_log)

            if process.returncode != 0:
                print("Client does not support --log argument. Logging disabled.")
                return False
            return True
        except Exception as e:
            # subprocess.communicate() does not kill the child on TimeoutExpired - without
            # this it would leak an orphaned client process still holding a server connection
            # open for every suite run.
            if process is not None and process.poll() is None:
                process.kill()
                process.communicate()
            print(f"Warning: Failed to check logging support: {e}")
            # Default to False if check fails to be safe
            return False

    def start_client_process(self, address, log_file=None, cwd=None, text=True, bufsize=-1):
        """
        Start the client process with appropriate arguments.
        Handles logging support check automatically.
        """
        if self.supports_logging is None:
            self.supports_logging = self._check_logging_support()
            
        # Use stdbuf to force unbuffered output for interactive tests
        args = ["stdbuf", "-o0", "-e0", CLIENT_EXE, address]
        if self.supports_logging and log_file:
            args.extend(["--log", log_file])
            
        return subprocess.Popen(
            args,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=text,
            bufsize=bufsize,
            cwd=cwd if cwd else self.client_cwd
        )

    def _signal_handler(self, signum, frame):
        self.cleanup()
        sys.exit(1)
    
    def _cleanup_client_metadata(self):
        """Remove client metadata files that could cause resume prompts."""
        metadata_files = [".minidrive_downloads.json", ".minidrive_uploads.json"]
        for fname in metadata_files:
            path = os.path.join(self.client_cwd, fname)
            if os.path.exists(path):
                os.remove(path)
        
    def setup_server_root(self):
        """Create a clean server root directory structure."""
        if os.path.exists(self.server_root):
            shutil.rmtree(self.server_root)
        os.makedirs(self.server_root, exist_ok=True)
    
    def start_server(self):
        """Start the MiniDrive server."""
        self.server_log_file = open(os.path.join(self.log_dir, "server.log"), "a")
        self.server_process = subprocess.Popen(
            [SERVER_EXE, "--port", str(self.port), "--root", self.server_root],
            stdout=self.server_log_file,
            stderr=self.server_log_file,
            text=True
        )
        time.sleep(0.5)
        if self.server_process.poll() is not None:
            self.server_log_file.close()
            with open(os.path.join(self.log_dir, "server.log"), "r") as f:
                output = f.read()
            raise RuntimeError(f"Server failed to start: {output}")
        print(f"Server started on port {self.port} (PID: {self.server_process.pid})")
    
    def stop_server(self, use_sigterm=True):
        """Stop the server process."""
        if self.server_process:
            if self.server_process.poll() is None:
                if use_sigterm:
                    self.server_process.send_signal(signal.SIGTERM)
                    try:
                        self.server_process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        self.server_process.kill()
                        self.server_process.wait()
                else:
                    self.server_process.kill()
                    self.server_process.wait()
            self.server_process = None

        if self.server_log_file:
            try:
                self.server_log_file.write("\n--- Server stopped ---\n")
            except Exception:
                pass
            try:
                self.server_log_file.close()
            except Exception:
                pass
            self.server_log_file = None
    
    def restart_server(self):
        """Restart the server process."""
        self.stop_server()
        time.sleep(0.5)
        self.start_server()

    def new_workdir(self, label: str) -> str:
        self.test_counter += 1
        work_dir = os.path.join(self.log_dir, f"{self.test_counter:02d}_{label}_workdir")
        os.makedirs(work_dir, exist_ok=True)
        return work_dir

    def connect_and_wait_for_register_prompt(self, username, label, timeout=5):
        """Connect and wait for the 'Register?' prompt."""
        cwd = self.new_workdir(label)
        stdout_log = os.path.join(self.log_dir, f"{label}_stdout.log")
        client_log = os.path.join(self.log_dir, f"{label}_client.log")
        
        # Use binary mode and unbuffered I/O to avoid TextIOWrapper buffering issues
        proc = self.start_client_process(f"{username}@127.0.0.1:{self.port}", client_log, cwd=cwd, text=False, bufsize=0)
        
        captured_stdout = ""
        start_time = time.time()
        found = False
        
        while time.time() - start_time < timeout:
            r, _, _ = select.select([proc.stdout], [], [], 0.1)
            if r:
                # Read bytes directly
                char_bytes = proc.stdout.read(1)
                if not char_bytes:
                    break # EOF
                
                # Decode and append
                try:
                    char = char_bytes.decode('utf-8')
                    captured_stdout += char
                except UnicodeDecodeError:
                    # Handle partial multi-byte characters if necessary, 
                    # but for ASCII prompts this shouldn't be an issue.
                    # Just ignore for now or accumulate bytes.
                    pass

                if "register" in captured_stdout.lower():
                    found = True
                    break
            
        if not found:
            proc.kill()
            with open(stdout_log, "w") as f:
                f.write(f"# TIMEOUT waiting for prompt\n{captured_stdout}")
            # Return None to signal failure without crashing immediately if caller handles it
            # But for now let's raise or return failure state
            return None, captured_stdout, stdout_log
            
        return proc, captured_stdout, stdout_log

    def complete_registration(self, proc, password, captured_stdout, stdout_log, timeout=30):
        """Send 'y' and password to complete registration."""
        # Input must be bytes because proc was started with text=False
        input_data = ("\n".join(["y", password]) + "\n").encode('utf-8')
        
        try:
            stdout_rest_bytes, _ = proc.communicate(input=input_data, timeout=timeout)
            code = proc.returncode
            stdout_rest = stdout_rest_bytes.decode('utf-8', errors='replace') if stdout_rest_bytes else ""
        except subprocess.TimeoutExpired:
            proc.kill()
            stdout_rest_bytes, _ = proc.communicate()
            stdout_rest = stdout_rest_bytes.decode('utf-8', errors='replace') if stdout_rest_bytes else ""
            code = -1
            
        full_stdout = captured_stdout + stdout_rest
        
        with open(stdout_log, "w") as f:
            f.write(f"# Exit code: {code}\n")
            f.write(f"# {'='*50}\n\n")
            f.write(full_stdout)
            
        return full_stdout, code

    def register_user(self, username: str, password: str, label: str, timeout: int = 30):
        """Register a new user (black-box via client)."""
        proc, captured, log_path = self.connect_and_wait_for_register_prompt(username, label, timeout=5)
        if proc is None:
            return captured, -1
        return self.complete_registration(proc, password, captured, log_path, timeout)

    def run_client(self, commands, test_name, timeout=30):
        """
        Run client commands and log output.
        Returns (stdout, exit_code).
        """
        self.test_counter += 1
        log_prefix = f"{self.test_counter:02d}_{test_name}"
        
        # Clean up metadata before each test to avoid resume prompts
        self._cleanup_client_metadata()
        
        # Prepare log files
        stdout_log = os.path.join(self.log_dir, f"{log_prefix}_stdout.log")
        client_log = os.path.join(self.log_dir, f"{log_prefix}_client.log")
        
        process = self.start_client_process(f"127.0.0.1:{self.port}", client_log)
        
        try:
            if isinstance(commands, list):
                input_data = "\n".join(commands) + "\nEXIT\n"
            else:
                input_data = f"{commands}\nEXIT\n"
            
            stdout, _ = process.communicate(input=input_data, timeout=timeout)
            exit_code = process.returncode
            
            # Write stdout log
            with open(stdout_log, "w") as f:
                f.write(f"# Commands: {commands}\n")
                f.write(f"# Exit code: {exit_code}\n")
                f.write(f"# {'='*50}\n\n")
                f.write(stdout)
            
            return stdout, exit_code
            
        except subprocess.TimeoutExpired:
            process.kill()
            with open(stdout_log, "w") as f:
                f.write(f"# Commands: {commands}\n")
                f.write(f"# TIMEOUT after {timeout}s\n")
            return "TIMEOUT", -1
    
    def cleanup(self):
        """Clean up test environment."""
        self.stop_server()
        if os.path.exists(self.server_root):
            shutil.rmtree(self.server_root, ignore_errors=True)
        if os.path.exists(self.client_cwd):
            shutil.rmtree(self.client_cwd, ignore_errors=True)
        # No need to call _cleanup_client_metadata as we removed the dir


class TestResult:
    """Test result tracker with logging."""
    
    def __init__(self, log_dir):
        self.passed = 0
        self.failed = 0
        self.errors = []
        self.log_dir = log_dir
        self.results_file = open(os.path.join(log_dir, "results.txt"), "w")
    
    def ok(self, name):
        print(f"  ✓ {name}")
        self.results_file.write(f"PASS: {name}\n")
        self.results_file.flush()
        self.passed += 1
    
    def fail(self, name, reason):
        print(f"  ✗ {name}: {reason}")
        self.results_file.write(f"FAIL: {name} - {reason}\n")
        self.results_file.flush()
        self.failed += 1
        self.errors.append((name, reason))
    
    def summary(self):
        total = self.passed + self.failed
        summary_text = f"\nResults: {self.passed}/{total} passed"
        print(f"\n{'='*50}")
        print(summary_text)
        self.results_file.write(f"\n{'='*50}\n")
        self.results_file.write(f"{summary_text}\n")
        
        if self.failed > 0:
            print("Failures:")
            self.results_file.write("Failures:\n")
            for name, reason in self.errors:
                print(f"  - {name}: {reason}")
                self.results_file.write(f"  - {name}: {reason}\n")
        
        self.results_file.close()
        return self.failed == 0


def check_executables():
    """Check that server and client executables exist."""
    if not os.path.exists(SERVER_EXE):
        print(f"ERROR: Server executable not found: {SERVER_EXE}")
        print("Run 'cmake --build build' first.")
        sys.exit(1)
    
    if not os.path.exists(CLIENT_EXE):
        print(f"ERROR: Client executable not found: {CLIENT_EXE}")
        print("Run 'cmake --build build' first.")
        sys.exit(1)
