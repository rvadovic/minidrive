#!/usr/bin/env python3
"""Integration tests for Client Logging.

Goal:
- Validate that the client supports the --log argument.
- Verify that operations (upload/download) produce log output.

Usage:
    python3 tests/integration/test_logging.py
"""

import os
import sys
import subprocess
import time
import shutil

from test_utils import (
    TestResult,
    check_executables,
    SERVER_EXE,
    CLIENT_EXE,
    TestEnvironment,
    calculate_hash,
    has_ok_response
)

SERVER_PORT = 9028


class LoggingEnv(TestEnvironment):
    def __init__(self, port=SERVER_PORT):
        super().__init__("logging", port)

    def new_workdir(self, label: str) -> str:
        self.test_counter += 1
        work_dir = os.path.join(self.log_dir, f"{self.test_counter:02d}_{label}_workdir")
        os.makedirs(work_dir, exist_ok=True)
        return work_dir


def test_logging_flag_supported(env: LoggingEnv, results: TestResult):
    """Check if the client accepts the --log argument."""
    
    # We can manually check this by trying to run the client with --log
    # and asserting it doesn't fail immediately or print usage error.
    
    log_file = os.path.join(env.log_dir, "flag_check.log")
    
    # We use subprocess directly here to verify the flag specifically, 
    # bypassing the helper which might mask it if we want to be explicit.
    # However, the helper uses _check_logging_support internally.
    # Let's verify that the environment detected support.
    
    # Force a check if not done
    if env.supports_logging is None:
        env.supports_logging = env._check_logging_support()
        
    if env.supports_logging:
        results.ok("Client supports --log argument")
    else:
        results.fail("Logging support", "Client does not support --log argument (or check failed)")


def test_logging_content(env: LoggingEnv, results: TestResult):
    """Perform upload/download and verify log file content."""
    
    if not env.supports_logging:
        results.fail("Logging content", "Skipping because logging is not supported")
        return

    workdir = env.new_workdir("log_content")
    filename = "test_log_activity.bin"
    local_path = os.path.join(workdir, filename)
    
    # Create a small file
    with open(local_path, "wb") as f:
        f.write(b"logging test content " * 100)
        
    log_file = os.path.join(env.log_dir, "activity.log")
    
    # Start client with logging
    # We use start_client_process which handles the --log arg if supported
    proc = env.start_client_process(f"127.0.0.1:{env.port}", log_file=log_file, cwd=workdir)
    
    commands = [
        f"UPLOAD {filename}",
        f"DOWNLOAD {filename} downloaded_{filename}",
        "EXIT"
    ]
    
    input_data = "\n".join(commands) + "\n"
    
    try:
        stdout, _ = proc.communicate(input=input_data, timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
        stdout, _ = proc.communicate()
        
    if not has_ok_response(stdout):
        results.fail("Logging content", "Client operations failed")
        return

    # Check if log file exists
    if not os.path.exists(log_file):
        results.fail("Logging content", f"Log file was not created: {log_file}")
        return
        
    # Check if log file has content
    file_size = os.path.getsize(log_file)
    if file_size == 0:
        results.fail("Logging content", "Log file is empty")
        return
        
    # Read log content and check for keywords (optional, but good verification)
    with open(log_file, "r") as f:
        content = f.read()
        
    print(f"  Log file size: {file_size} bytes")
    
    # We expect to see something related to the operations or protocol
    # Since we don't know the exact log format, just checking it's not empty is the primary goal.
    # But usually logs contain timestamps or command names.
    
    results.ok("Log file created and contains content")


def main():
    print("MiniDrive Integration Tests - Client Logging")
    print("=" * 60)

    check_executables()

    env = LoggingEnv()
    results = TestResult(env.log_dir)

    try:
        env.setup_server_root()
        env.start_server()

        test_logging_flag_supported(env, results)
        test_logging_content(env, results)

    finally:
        env.cleanup()

    ok = results.summary()
    print(f"\nLogs saved to: {env.log_dir}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
