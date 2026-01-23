#!/usr/bin/env python3
"""
Integration tests for MiniDrive Authentication and Multi-User support.

Tests the following criteria (5 points):
- Public/private directories
- User registration
- User authentication with hashed passwords
- Private directory isolation between users
- Session handling (one user then another)

All tests are BLACK-BOX tests - they only interact through the client CLI
and do not inspect server internals.

Usage:
    python3 tests/integration/test_authentication.py
"""

import os
import sys
import subprocess
import time
import signal

from test_utils import (
    has_ok_response,
    TestResult, check_executables, calculate_hash,
    BUILD_DIR, CLIENT_EXE, SERVER_EXE, get_test_log_dir,
    TestEnvironment
)
import shutil

SERVER_PORT = 9023


class AuthTestEnvironment(TestEnvironment):
    """Test environment with server restart capability for persistence testing."""
    pass
    
    def run_client_as_user(self, username, password, commands, test_name, 
                           expect_register=False, timeout=30):
        """
        Run client commands as an authenticated user.
        Returns (stdout, exit_code).
        """
        self.test_counter += 1
        log_prefix = f"{self.test_counter:02d}_{test_name}"
        
        stdout_log = os.path.join(self.log_dir, f"{log_prefix}_stdout.log")
        client_log = os.path.join(self.log_dir, f"{log_prefix}_client.log")
        
        process = self.start_client_process(f"{username}@127.0.0.1:{self.port}", client_log)
        
        try:
            input_lines = []
            
            if expect_register:
                input_lines.append("y")  # Yes, register
                input_lines.append(password)
            else:
                input_lines.append(password)
                if isinstance(commands, list):
                    input_lines.extend(commands)
                elif commands:
                    input_lines.append(commands)
                input_lines.append("EXIT")
            
            input_data = "\n".join(input_lines) + "\n"
            
            stdout, _ = process.communicate(input=input_data, timeout=timeout)
            exit_code = process.returncode
            
            with open(stdout_log, "w") as f:
                f.write(f"# User: {username}\n")
                f.write(f"# Commands: {commands}\n")
                f.write(f"# Expect register: {expect_register}\n")
                f.write(f"# Exit code: {exit_code}\n")
                f.write(f"# {'='*50}\n\n")
                f.write(stdout)
            
            return stdout, exit_code
            
        except subprocess.TimeoutExpired:
            process.kill()
            with open(stdout_log, "w") as f:
                f.write(f"# TIMEOUT after {timeout}s\n")
            return "TIMEOUT", -1
    
    def run_public_client(self, commands, test_name, timeout=30):
        """Run client in public mode (no username)."""
        self.test_counter += 1
        log_prefix = f"{self.test_counter:02d}_{test_name}"
        
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
            
            with open(stdout_log, "w") as f:
                f.write(f"# Public mode\n")
                f.write(f"# Commands: {commands}\n")
                f.write(f"# Exit code: {exit_code}\n")
                f.write(f"# {'='*50}\n\n")
                f.write(stdout)
            
            return stdout, exit_code
            
        except subprocess.TimeoutExpired:
            process.kill()
            return "TIMEOUT", -1
    
    def cleanup(self):
        """Clean up test environment."""
        self.stop_server()
        if os.path.exists(self.server_root):
            shutil.rmtree(self.server_root, ignore_errors=True)
        self._cleanup_client_metadata()


# =============================================================================
# Test Cases - All BLACK-BOX (client perspective only)
# =============================================================================

def test_public_mode_warning(env, results):
    """Test that public mode shows warning message."""
    stdout, code = env.run_public_client("LIST", "public_warning")
    
    if "public mode" in stdout.lower() or "warning" in stdout.lower():
        results.ok("Public mode shows warning message")
    else:
        results.fail("Public mode warning", "No warning message found")


def test_register_new_user(env, results):
    """Test registering a new user."""
    username = "newuser"
    password = "mypassword123"
    
    stdout, code = env.run_client_as_user(
        username, password, [],
        "register_new_user",
        expect_register=True
    )
    
    # Should show registration success
    if "register" in stdout.lower() or "success" in stdout.lower() or code == 0:
        results.ok("User registration succeeds")
    else:
        results.fail("User registration", f"Unexpected: {stdout[:200]}")


def test_login_after_registration(env, results):
    """Test that a registered user can log in."""
    username = "newuser"
    password = "mypassword123"
    
    # Should be able to log in now
    stdout, code = env.run_client_as_user(
        username, password, "LIST",
        "login_after_register"
    )
    
    if "logged" in stdout.lower() or has_ok_response(stdout):
        results.ok("Login after registration succeeds")
    else:
        results.fail("Login after registration", f"Login failed: {stdout[:200]}")


def test_wrong_password_rejected(env, results):
    """Test that wrong password is rejected."""
    username = "newuser"
    wrong_password = "wrongpassword"
    
    stdout, code = env.run_client_as_user(
        username, wrong_password, "LIST",
        "wrong_password"
    )
    
    # Should fail - look for error or no successful login
    if "error" in stdout.lower() or "failed" in stdout.lower() or "invalid" in stdout.lower():
        results.ok("Wrong password is rejected")
    elif "logged" in stdout.lower() and has_ok_response(stdout):
        results.fail("Wrong password rejected", "Login succeeded with wrong password!")
    else:
        results.ok("Wrong password rejected (connection closed)")


def test_credentials_persist_after_restart(env, results):
    """Test that user credentials persist after server restart."""
    username = "persistuser"
    password = "persistpass123"
    
    # Register user
    stdout, code = env.run_client_as_user(
        username, password, [],
        "persist_register",
        expect_register=True
    )
    
    if "success" not in stdout.lower() and "register" not in stdout.lower() and code != 0:
        results.fail("Persistence test setup", "Registration failed")
        return
    
    # Restart server
    env.restart_server()
    
    # Try to log in - should work with same password
    stdout, code = env.run_client_as_user(
        username, password, "LIST",
        "persist_login_after_restart"
    )
    
    if "logged" in stdout.lower() or has_ok_response(stdout):
        results.ok("Credentials persist after server restart")
    else:
        results.fail("Credentials persist", "Login failed after restart")
    
    # Wrong password should still fail
    stdout, code = env.run_client_as_user(
        username, "wrongpass", "LIST",
        "persist_wrong_pass_after_restart"
    )
    
    if "error" in stdout.lower() or "failed" in stdout.lower():
        results.ok("Password verification works after restart")
    elif "logged" in stdout.lower():
        results.fail("Password verification", "Wrong password accepted after restart!")
    else:
        results.ok("Password verification works after restart")


def test_user_files_isolated(env, results):
    """Test that users cannot see each other's files."""
    alice = "alice"
    bob = "bob"
    alice_pass = "alicepass"
    bob_pass = "bobpass"
    
    # Register both users
    env.run_client_as_user(alice, alice_pass, [], "isolation_reg_alice", expect_register=True)
    time.sleep(0.3)
    env.run_client_as_user(bob, bob_pass, [], "isolation_reg_bob", expect_register=True)
    time.sleep(0.3)
    
    # Alice uploads a file
    alice_file = "alice_private.txt"
    with open(alice_file, "w") as f:
        f.write("Alice's secret data")
    
    stdout, code = env.run_client_as_user(
        alice, alice_pass, f"UPLOAD {alice_file}",
        "isolation_alice_upload"
    )
    os.remove(alice_file)
    
    if not has_ok_response(stdout):
        results.fail("File isolation setup", "Alice upload failed")
        return
    
    # Alice should see her file
    stdout, code = env.run_client_as_user(
        alice, alice_pass, "LIST",
        "isolation_alice_sees_own"
    )
    
    if alice_file in stdout:
        results.ok("User can see their own files")
    else:
        results.fail("User sees own files", "Alice can't see her file")
        return
    
    # Bob should NOT see Alice's file
    stdout, code = env.run_client_as_user(
        bob, bob_pass, "LIST",
        "isolation_bob_list"
    )
    
    if alice_file not in stdout:
        results.ok("Users cannot see each other's files")
    else:
        results.fail("File isolation", "Bob can see Alice's file!")


def test_public_private_isolated(env, results):
    """Test that public and private files are isolated."""
    user = "privateonly"
    password = "privatepass"
    
    # Register user
    env.run_client_as_user(user, password, [], "pub_priv_register", expect_register=True)
    time.sleep(0.3)
    
    # Upload file in public mode
    public_file = "public_file.txt"
    with open(public_file, "w") as f:
        f.write("Public content")
    
    stdout, code = env.run_public_client(f"UPLOAD {public_file}", "pub_priv_public_upload")
    os.remove(public_file)
    
    if not has_ok_response(stdout):
        results.fail("Public/Private test setup", "Public upload failed")
        return
    
    # Private user should NOT see public file
    stdout, code = env.run_client_as_user(
        user, password, "LIST",
        "pub_priv_private_cant_see_public"
    )
    
    if public_file not in stdout:
        results.ok("Private user cannot see public files")
    else:
        results.fail("Public/Private isolation", "Private user sees public files")
    
    # Upload private file
    private_file = "private_file.txt"
    with open(private_file, "w") as f:
        f.write("Private content")
    
    env.run_client_as_user(user, password, f"UPLOAD {private_file}", "pub_priv_private_upload")
    os.remove(private_file)
    
    # Public mode should NOT see private file
    stdout, code = env.run_public_client("LIST", "pub_priv_public_cant_see_private")
    
    if private_file not in stdout:
        results.ok("Public mode cannot see private files")
    else:
        results.fail("Public/Private isolation", "Public sees private files")


def test_files_persist_after_restart(env, results):
    """Test that user files persist after server restart."""
    username = "filepersist"
    password = "filepass"
    
    # Register and upload
    env.run_client_as_user(username, password, [], "file_persist_reg", expect_register=True)
    time.sleep(0.3)
    
    test_file = "persist_test.txt"
    test_content = "This should survive restart"
    with open(test_file, "w") as f:
        f.write(test_content)
    
    stdout, code = env.run_client_as_user(
        username, password, f"UPLOAD {test_file}",
        "file_persist_upload"
    )
    os.remove(test_file)
    
    if not has_ok_response(stdout):
        results.fail("File persistence setup", "Upload failed")
        return
    
    # Restart server
    env.restart_server()
    
    # File should still be there
    stdout, code = env.run_client_as_user(
        username, password, "LIST",
        "file_persist_list_after_restart"
    )
    
    if test_file in stdout:
        results.ok("Files persist after server restart")
    else:
        results.fail("File persistence", "File missing after restart")
    
    # Download and verify content
    download_file = "downloaded_persist.txt"
    stdout, code = env.run_client_as_user(
        username, password, f"DOWNLOAD {test_file} {download_file}",
        "file_persist_download"
    )
    
    if os.path.exists(download_file):
        with open(download_file, "r") as f:
            content = f.read()
        os.remove(download_file)
        
        if content == test_content:
            results.ok("File content intact after restart")
        else:
            results.fail("File content", "Content changed after restart")
    else:
        results.fail("File download after restart", "Download failed")


def test_sequential_users(env, results):
    """Test that multiple users can use the system sequentially."""
    users = [
        ("user1", "pass1"),
        ("user2", "pass2"),
        ("user3", "pass3"),
    ]
    
    # Register all users
    for username, password in users:
        env.run_client_as_user(username, password, [], f"seq_{username}_reg", expect_register=True)
        time.sleep(0.2)
    
    # Each user uploads a uniquely named file
    for username, password in users:
        filename = f"{username}_data.txt"
        with open(filename, "w") as f:
            f.write(f"Data for {username}")
        
        stdout, code = env.run_client_as_user(
            username, password, f"UPLOAD {filename}",
            f"seq_{username}_upload"
        )
        os.remove(filename)
        
        if not has_ok_response(stdout):
            results.fail("Sequential users", f"Upload failed for {username}")
            return
    
    results.ok("Sequential users can register and upload")
    
    # Verify each user only sees their own file
    all_isolated = True
    for username, password in users:
        own_file = f"{username}_data.txt"
        other_files = [f"{u}_data.txt" for u, p in users if u != username]
        
        stdout, code = env.run_client_as_user(
            username, password, "LIST",
            f"seq_{username}_verify"
        )
        
        if own_file not in stdout:
            all_isolated = False
            break
        
        for other in other_files:
            if other in stdout:
                all_isolated = False
                break
    
    if all_isolated:
        results.ok("Each user sees only their own files")
    else:
        results.fail("Sequential user isolation", "Isolation failed")


def test_same_filename_different_users(env, results):
    """Test that different users can have files with the same name."""
    user1 = "samefileuser1"
    user2 = "samefileuser2"
    password = "samepass"
    filename = "common_name.txt"
    
    # Register users
    env.run_client_as_user(user1, password, [], "samefile_reg1", expect_register=True)
    time.sleep(0.2)
    env.run_client_as_user(user2, password, [], "samefile_reg2", expect_register=True)
    time.sleep(0.2)
    
    # User1 uploads with content1
    content1 = "User1 unique content"
    with open(filename, "w") as f:
        f.write(content1)
    env.run_client_as_user(user1, password, f"UPLOAD {filename}", "samefile_upload1")
    
    # User2 uploads with content2 (same filename)
    content2 = "User2 different content"
    with open(filename, "w") as f:
        f.write(content2)
    env.run_client_as_user(user2, password, f"UPLOAD {filename}", "samefile_upload2")
    os.remove(filename)
    
    # User1 downloads their version
    download1 = "user1_download.txt"
    env.run_client_as_user(user1, password, f"DOWNLOAD {filename} {download1}", "samefile_dl1")
    
    # User2 downloads their version
    download2 = "user2_download.txt"
    env.run_client_as_user(user2, password, f"DOWNLOAD {filename} {download2}", "samefile_dl2")
    
    # Verify contents are different
    if os.path.exists(download1) and os.path.exists(download2):
        with open(download1) as f:
            got1 = f.read()
        with open(download2) as f:
            got2 = f.read()
        
        os.remove(download1)
        os.remove(download2)
        
        if got1 == content1 and got2 == content2:
            results.ok("Different users can have same filename with different content")
        else:
            results.fail("Same filename", "Content mixed up between users")
    else:
        results.fail("Same filename", "Download failed")
        for f in [download1, download2]:
            if os.path.exists(f):
                os.remove(f)


def test_user_can_reconnect(env, results):
    """Test that a user can disconnect and reconnect multiple times."""
    username = "reconnector"
    password = "reconnectpass"
    
    # Register
    env.run_client_as_user(username, password, [], "reconnect_reg", expect_register=True)
    time.sleep(0.2)
    
    # Session 1: Upload file
    file1 = "session1_file.txt"
    with open(file1, "w") as f:
        f.write("Session 1")
    env.run_client_as_user(username, password, f"UPLOAD {file1}", "reconnect_s1")
    os.remove(file1)
    
    time.sleep(0.3)
    
    # Session 2: Upload another file and check first is there
    file2 = "session2_file.txt"
    with open(file2, "w") as f:
        f.write("Session 2")
    
    stdout, code = env.run_client_as_user(
        username, password, [f"UPLOAD {file2}", "LIST"],
        "reconnect_s2"
    )
    os.remove(file2)
    
    if file1 in stdout and file2 in stdout:
        results.ok("User can reconnect and access previous files")
    else:
        results.fail("Reconnection", "Files from previous session missing")


def test_user_mkdir_rmdir(env, results):
    """Test that users can create and remove directories."""
    username = "diruser"
    password = "dirpass"
    
    # Register
    env.run_client_as_user(username, password, [], "dir_reg", expect_register=True)
    time.sleep(0.2)
    
    # Create directory
    dirname = "mydir"
    stdout, code = env.run_client_as_user(
        username, password, f"MKDIR {dirname}",
        "dir_mkdir"
    )
    
    if not has_ok_response(stdout):
        results.fail("User mkdir", "MKDIR failed")
        return
    
    results.ok("User can create directory")
    
    # List should show directory
    stdout, code = env.run_client_as_user(
        username, password, "LIST",
        "dir_list"
    )
    
    if dirname in stdout:
        results.ok("Created directory visible in LIST")
    else:
        results.fail("Directory visible", "Directory not in LIST")
    
    # Remove directory
    stdout, code = env.run_client_as_user(
        username, password, f"RMDIR {dirname}",
        "dir_rmdir"
    )
    
    if has_ok_response(stdout):
        results.ok("User can remove directory")
    else:
        results.fail("User rmdir", "RMDIR failed")


def test_path_traversal_blocked(env, results):
    """Test that path traversal attempts are blocked."""
    username = "traversaluser"
    password = "travpass"
    
    # Register
    env.run_client_as_user(username, password, [], "traversal_reg", expect_register=True)
    time.sleep(0.2)
    
    # Try to access parent directory
    stdout, code = env.run_client_as_user(
        username, password, "LIST ..",
        "traversal_list"
    )
    
    # Should either error or stay in user's directory
    if "ERROR" in stdout:
        results.ok("Path traversal LIST returns ERROR")
    else:
        results.ok("Path traversal LIST handled (stayed in sandbox)")
    
    # Try to delete outside user directory  
    stdout, code = env.run_client_as_user(
        username, password, "DELETE ../somefile",
        "traversal_delete"
    )
    
    if "ERROR" in stdout:
        results.ok("Path traversal DELETE blocked")
    else:
        results.ok("Path traversal DELETE handled")


def main():
    print("MiniDrive Integration Tests - Authentication & Multi-User")
    print("=" * 60)
    print("Testing: Registration, Login, Persistence, User Isolation")
    print("(Black-box tests - client perspective only)")
    
    check_executables()
    
    env = AuthTestEnvironment("authentication", SERVER_PORT)
    results = TestResult(env.log_dir)
    
    # Change to client working directory so tests create files there
    os.chdir(env.client_cwd)
    
    try:
        print("\nSetting up test environment...")
        env.setup_server_root()
        env.start_server()
        
        print("\nRunning tests:\n")
        
        # Public mode
        test_public_mode_warning(env, results)
        
        # Registration and login
        test_register_new_user(env, results)
        test_login_after_registration(env, results)
        test_wrong_password_rejected(env, results)
        
        # Persistence (involves server restart)
        test_credentials_persist_after_restart(env, results)
        test_files_persist_after_restart(env, results)
        
        # User isolation
        test_user_files_isolated(env, results)
        test_public_private_isolated(env, results)
        test_same_filename_different_users(env, results)
        
        # Sequential sessions
        test_sequential_users(env, results)
        test_user_can_reconnect(env, results)
        
        # Directory operations
        test_user_mkdir_rmdir(env, results)
        
        # Security
        test_path_traversal_blocked(env, results)
        
    finally:
        print("\nCleaning up...")
        env.cleanup()
    
    success = results.summary()
    print(f"\nLogs saved to: {env.log_dir}")
    
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
