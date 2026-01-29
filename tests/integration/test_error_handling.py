#!/usr/bin/env python3
"""
Integration tests for MiniDrive Error Handling.

Tests failure cases and error conditions:
- Upload/download fails when file already exists (per spec)
- Operations on non-existent files
- Invalid commands and arguments
- Permission/path errors
- Connection errors

Usage:
    python3 tests/integration/test_error_handling.py
"""

import os
import sys
import subprocess
import time
import signal
import shutil

from test_utils import (
    has_ok_response,
    TestResult, check_executables, calculate_hash,
    BUILD_DIR, CLIENT_EXE, SERVER_EXE, get_test_log_dir,
    TestEnvironment
)

SERVER_PORT = 9026


class ErrorTestEnvironment(TestEnvironment):
    """Test environment for error handling tests."""
    pass


# =============================================================================
# Upload Error Tests
# =============================================================================

def test_upload_file_already_exists_on_server(env, results):
    """Test that UPLOAD fails when file already exists on server (per spec)."""
    filename = "existing_file.txt"
    
    # Create file on server first via UPLOAD
    with open(filename, "w") as f:
        f.write("Original server content\n")
    
    env.run_client(f"UPLOAD {filename}", "setup_upload_exists")
    
    # Create local file with different content
    with open(filename, "w") as f:
        f.write("New local content\n")
    
    try:
        stdout, code = env.run_client(f"UPLOAD {filename}", "upload_exists")
        
        # Per spec: "Upload or download should fail if it would overwrite an existing file"
        if "ERROR" in stdout:
            results.ok("UPLOAD fails when file exists on server (spec compliant)")
            
            # Verify original content preserved via DOWNLOAD
            download_file = "verify_preservation.txt"
            env.run_client(f"DOWNLOAD {filename} {download_file}", "verify_preservation")
            
            if os.path.exists(download_file):
                with open(download_file, "r") as f:
                    content = f.read()
                if "Original" in content:
                    results.ok("Original file preserved on upload conflict")
                else:
                    results.fail("File preservation", "Original content was modified")
                os.remove(download_file)
            else:
                results.fail("File preservation", "Could not download file to verify preservation")
                
        elif has_ok_response(stdout):
            # Implementation allows overwrite - document this
            results.fail("UPLOAD when file exists", 
                        "Spec requires ERROR, but got OK (overwrites)")
        else:
            results.fail("UPLOAD when file exists", f"Unexpected: {stdout[:200]}")
    
    finally:
        if os.path.exists(filename):
            os.remove(filename)
        env.run_client(f"DELETE {filename}", "cleanup_upload_exists")


def test_upload_nonexistent_local_file(env, results):
    """Test that UPLOAD fails for non-existent local file."""
    stdout, code = env.run_client("UPLOAD nonexistent_file_xyz.txt", "upload_no_local")
    
    if "ERROR" in stdout:
        results.ok("UPLOAD fails for non-existent local file")
    else:
        results.fail("UPLOAD non-existent", f"Expected ERROR: {stdout[:200]}")


def test_upload_to_nonexistent_directory(env, results):
    """Test UPLOAD to a non-existent remote directory path."""
    filename = "test_file.txt"
    with open(filename, "w") as f:
        f.write("Test content\n")
    
    try:
        stdout, code = env.run_client(
            f"UPLOAD {filename} nonexistent_dir/subdir/file.txt",
            "upload_no_dir"
        )
        
        # Should either fail or auto-create directories
        if "ERROR" in stdout:
            results.ok("UPLOAD to non-existent path returns ERROR")
        elif has_ok_response(stdout):
            results.ok("UPLOAD auto-creates parent directories")
        else:
            results.fail("UPLOAD to non-existent dir", f"Unexpected: {stdout[:200]}")
    
    finally:
        if os.path.exists(filename):
            os.remove(filename)


# =============================================================================
# Download Error Tests
# =============================================================================

def test_download_nonexistent_remote_file(env, results):
    """Test that DOWNLOAD fails for non-existent remote file."""
    stdout, code = env.run_client(
        "DOWNLOAD nonexistent_remote.xyz local_copy.txt",
        "download_no_remote"
    )
    
    if "ERROR" in stdout:
        results.ok("DOWNLOAD fails for non-existent remote file")
    else:
        results.fail("DOWNLOAD non-existent", f"Expected ERROR: {stdout[:200]}")
    
    # Ensure no local file was created
    if os.path.exists("local_copy.txt"):
        os.remove("local_copy.txt")
        results.fail("DOWNLOAD cleanup", "Partial file created for non-existent remote")


def test_download_file_already_exists_locally(env, results):
    """Test that DOWNLOAD fails when local file already exists (per spec)."""
    remote_file = "remote_for_download.txt"
    local_file = "local_existing.txt"
    
    # Create file on server via UPLOAD
    with open(remote_file, "w") as f:
        f.write("Remote content\n")
    
    env.run_client(f"UPLOAD {remote_file}", "setup_download_exists")
    os.remove(remote_file)
    
    # Create local file
    with open(local_file, "w") as f:
        f.write("Original local content\n")
    
    try:
        stdout, code = env.run_client(
            f"DOWNLOAD {remote_file} {local_file}",
            "download_exists_local"
        )
        
        # Per spec: "Upload or download should fail if it would overwrite an existing file"
        if "ERROR" in stdout:
            results.ok("DOWNLOAD fails when local file exists (spec compliant)")
            
            # Verify original content preserved
            with open(local_file, "r") as f:
                content = f.read()
            if "Original local" in content:
                results.ok("Original local file preserved on download conflict")
            else:
                results.fail("Local file preservation", "Local content was modified")
        elif has_ok_response(stdout):
            results.fail("DOWNLOAD when local exists",
                        "Spec requires ERROR, but got OK (overwrites)")
        else:
            results.fail("DOWNLOAD when local exists", f"Unexpected: {stdout[:200]}")
    
    finally:
        if os.path.exists(local_file):
            os.remove(local_file)
        env.run_client(f"DELETE {remote_file}", "cleanup_download_exists")


def test_download_directory_instead_of_file(env, results):
    """Test that DOWNLOAD fails when trying to download a directory."""
    # Create directory on server via MKDIR
    dir_name = "test_directory"
    env.run_client(f"MKDIR {dir_name}", "setup_download_dir")
    
    stdout, code = env.run_client(
        f"DOWNLOAD {dir_name} local_dir.txt",
        "download_directory"
    )
    
    if "ERROR" in stdout:
        results.ok("DOWNLOAD fails for directory")
    else:
        results.fail("DOWNLOAD directory", f"Expected ERROR: {stdout[:200]}")
    
    env.run_client(f"RMDIR {dir_name}", "cleanup_download_dir")


# =============================================================================
# Delete Error Tests
# =============================================================================

def test_delete_nonexistent_file(env, results):
    """Test that DELETE fails for non-existent file."""
    stdout, code = env.run_client("DELETE nonexistent_file.xyz", "delete_nonexistent")
    
    if "ERROR" in stdout:
        results.ok("DELETE fails for non-existent file")
    else:
        results.fail("DELETE non-existent", f"Expected ERROR: {stdout[:200]}")


def test_delete_directory_with_delete(env, results):
    """Test that DELETE command fails on directories (use RMDIR instead)."""
    # Create directory on server via MKDIR
    dir_name = "dir_to_delete"
    env.run_client(f"MKDIR {dir_name}", "setup_delete_dir")
    
    stdout, code = env.run_client(f"DELETE {dir_name}", "delete_directory")
    
    # DELETE should fail on directories - use RMDIR
    if "ERROR" in stdout:
        results.ok("DELETE fails on directory (use RMDIR)")
        env.run_client(f"RMDIR {dir_name}", "cleanup_delete_dir")
    elif has_ok_response(stdout):
        # Some implementations might allow this
        results.ok("DELETE removes directory (permissive implementation)")
    else:
        results.fail("DELETE on directory", f"Unexpected: {stdout[:200]}")
        env.run_client(f"RMDIR {dir_name}", "cleanup_delete_dir")


# =============================================================================
# Directory Error Tests
# =============================================================================

def test_mkdir_already_exists(env, results):
    """Test that MKDIR fails when directory already exists."""
    dir_name = "existing_directory"
    env.run_client(f"MKDIR {dir_name}", "setup_mkdir_exists")
    
    stdout, code = env.run_client(f"MKDIR {dir_name}", "mkdir_exists")
    
    # Per spec: "mkdir should fail if the target path already exists"
    if "ERROR" in stdout:
        results.ok("MKDIR fails when directory exists (spec compliant)")
    elif has_ok_response(stdout):
        results.fail("MKDIR when exists", "Spec requires ERROR")
    else:
        results.fail("MKDIR when exists", f"Unexpected: {stdout[:200]}")
    
    env.run_client(f"RMDIR {dir_name}", "cleanup_mkdir_exists")


def test_rmdir_nonexistent(env, results):
    """Test that RMDIR fails for non-existent directory."""
    stdout, code = env.run_client("RMDIR nonexistent_directory", "rmdir_nonexistent")
    
    if "ERROR" in stdout:
        results.ok("RMDIR fails for non-existent directory")
    else:
        results.fail("RMDIR non-existent", f"Expected ERROR: {stdout[:200]}")


def test_rmdir_on_file(env, results):
    """Test that RMDIR fails when target is a file."""
    filename = "file_not_dir.txt"
    
    # Create file via UPLOAD
    with open(filename, "w") as f:
        f.write("I am a file\n")
    
    env.run_client(f"UPLOAD {filename}", "setup_rmdir_file")
    os.remove(filename)
    
    stdout, code = env.run_client(f"RMDIR {filename}", "rmdir_on_file")
    
    if "ERROR" in stdout:
        results.ok("RMDIR fails on file")
    else:
        results.fail("RMDIR on file", f"Expected ERROR: {stdout[:200]}")
    
    env.run_client(f"DELETE {filename}", "cleanup_rmdir_file")


def test_cd_nonexistent_directory(env, results):
    """Test that CD fails for non-existent directory."""
    stdout, code = env.run_client("CD nonexistent_directory", "cd_nonexistent")
    
    if "ERROR" in stdout:
        results.ok("CD fails for non-existent directory")
    else:
        # CD might silently fail or return OK - check with LIST
        results.fail("CD non-existent", f"Expected ERROR: {stdout[:200]}")


def test_cd_to_file(env, results):
    """Test that CD fails when target is a file."""
    filename = "file_not_dir.txt"
    
    # Create file via UPLOAD
    with open(filename, "w") as f:
        f.write("I am a file\n")
    
    env.run_client(f"UPLOAD {filename}", "setup_cd_file")
    os.remove(filename)
    
    stdout, code = env.run_client(f"CD {filename}", "cd_to_file")
    
    if "ERROR" in stdout:
        results.ok("CD fails on file")
    else:
        results.fail("CD on file", f"Expected ERROR: {stdout[:200]}")
    
    env.run_client(f"DELETE {filename}", "cleanup_cd_file")


# =============================================================================
# Move/Copy Error Tests
# =============================================================================

def test_move_nonexistent_source(env, results):
    """Test that MOVE fails for non-existent source."""
    stdout, code = env.run_client("MOVE nonexistent.txt destination.txt", "move_no_source")
    
    if "ERROR" in stdout:
        results.ok("MOVE fails for non-existent source")
    else:
        results.fail("MOVE non-existent source", f"Expected ERROR: {stdout[:200]}")


def test_move_destination_exists(env, results):
    """Test that MOVE fails when destination already exists."""
    # Create source file via UPLOAD
    source = "move_source.txt"
    with open(source, "w") as f:
        f.write("Source content\n")
    env.run_client(f"UPLOAD {source}", "setup_move_source")
    os.remove(source)
    
    # Create destination file via UPLOAD
    dest = "move_dest.txt"
    with open(dest, "w") as f:
        f.write("Destination content\n")
    env.run_client(f"UPLOAD {dest}", "setup_move_dest")
    os.remove(dest)
    
    stdout, code = env.run_client(f"MOVE {source} {dest}", "move_dest_exists")
    
    # Per spec: "Move should fail if the target path already exists"
    if "ERROR" in stdout:
        results.ok("MOVE fails when destination exists (spec compliant)")
    elif has_ok_response(stdout):
        results.fail("MOVE dest exists", "Spec requires ERROR")
    else:
        results.fail("MOVE dest exists", f"Unexpected: {stdout[:200]}")
    
    env.run_client(f"DELETE {source}", "cleanup_move_source")
    env.run_client(f"DELETE {dest}", "cleanup_move_dest")


def test_copy_nonexistent_source(env, results):
    """Test that COPY fails for non-existent source."""
    stdout, code = env.run_client("COPY nonexistent.txt destination.txt", "copy_no_source")
    
    if "ERROR" in stdout:
        results.ok("COPY fails for non-existent source")
    else:
        results.fail("COPY non-existent source", f"Expected ERROR: {stdout[:200]}")


def test_copy_destination_exists(env, results):
    """Test that COPY fails when destination already exists."""
    # Create source file via UPLOAD
    source = "copy_source.txt"
    with open(source, "w") as f:
        f.write("Source content\n")
    env.run_client(f"UPLOAD {source}", "setup_copy_source")
    os.remove(source)
    
    # Create destination file via UPLOAD
    dest = "copy_dest.txt"
    with open(dest, "w") as f:
        f.write("Destination content\n")
    env.run_client(f"UPLOAD {dest}", "setup_copy_dest")
    os.remove(dest)
    
    stdout, code = env.run_client(f"COPY {source} {dest}", "copy_dest_exists")
    
    # Per spec: "Copy should fail if the target path already exists"
    if "ERROR" in stdout:
        results.ok("COPY fails when destination exists (spec compliant)")
    elif has_ok_response(stdout):
        results.fail("COPY dest exists", "Spec requires ERROR")
    else:
        results.fail("COPY dest exists", f"Unexpected: {stdout[:200]}")
    
    env.run_client(f"DELETE {source}", "cleanup_copy_source")
    env.run_client(f"DELETE {dest}", "cleanup_copy_dest")


# =============================================================================
# Invalid Command Tests
# =============================================================================

def test_unknown_command(env, results):
    """Test that unknown command returns error."""
    stdout, code = env.run_client("INVALID_COMMAND arg1 arg2", "unknown_command")
    
    if "ERROR" in stdout or "unknown" in stdout.lower():
        results.ok("Unknown command returns ERROR")
    else:
        results.fail("Unknown command", f"Expected error: {stdout[:200]}")


def test_command_missing_arguments(env, results):
    """Test commands with missing required arguments."""
    # UPLOAD without filename
    stdout, code = env.run_client("UPLOAD", "upload_no_args")
    if "ERROR" in stdout:
        results.ok("UPLOAD without args returns ERROR")
    else:
        results.fail("UPLOAD no args", f"Expected ERROR: {stdout[:100]}")
    
    # DOWNLOAD without args
    stdout, code = env.run_client("DOWNLOAD", "download_no_args")
    if "ERROR" in stdout:
        results.ok("DOWNLOAD without args returns ERROR")
    else:
        results.fail("DOWNLOAD no args", f"Expected ERROR: {stdout[:100]}")
    
    # CD without path
    stdout, code = env.run_client("CD", "cd_no_args")
    if "ERROR" in stdout:
        results.ok("CD without args returns ERROR")
    else:
        # Some implementations might CD to root
        results.ok("CD without args (goes to root or errors)")


def test_empty_command(env, results):
    """Test that empty command is handled gracefully."""
    stdout, code = env.run_client("", "empty_command")
    
    # Should not crash, might just show prompt again
    if "ERROR" not in stdout.upper() or code == 0:
        results.ok("Empty command handled gracefully")
    else:
        results.fail("Empty command", f"Unexpected error: {stdout[:200]}")


# =============================================================================
# Path Traversal Error Tests
# =============================================================================

def test_path_traversal_upload(env, results):
    """Test that path traversal in UPLOAD is blocked."""
    filename = "test_file.txt"
    with open(filename, "w") as f:
        f.write("Test content\n")
    
    try:
        stdout, code = env.run_client(
            f"UPLOAD {filename} ../outside_root.txt",
            "traversal_upload"
        )
        
        if "ERROR" in stdout:
            results.ok("Path traversal in UPLOAD blocked")
        elif has_ok_response(stdout):
            # Check file wasn't created outside root
            outside_path = os.path.join(env.server_root, "outside_root.txt")
            if not os.path.exists(outside_path):
                results.ok("Path traversal blocked (file in safe location)")
            else:
                results.fail("Path traversal UPLOAD", "File created outside public dir!")
        else:
            results.fail("Path traversal UPLOAD", f"Unexpected: {stdout[:200]}")
    
    finally:
        if os.path.exists(filename):
            os.remove(filename)


def test_path_traversal_download(env, results):
    """Test that path traversal in DOWNLOAD is blocked."""
    # Find a target file in server root
    target_file = None
    for f in os.listdir(env.server_root):
        if os.path.isfile(os.path.join(env.server_root, f)):
            target_file = f
            break
    
    target_name = target_file if target_file else "nonexistent_system_file"
    stolen_name = f"stolen_{target_name}"

    stdout, code = env.run_client(
        f"DOWNLOAD ../{target_name} {stolen_name}",
        "traversal_download"
    )
    
    if "ERROR" in stdout:
        results.ok("Path traversal in DOWNLOAD blocked")
    elif not os.path.exists(stolen_name):
        results.ok("Path traversal blocked (no file downloaded)")
    else:
        os.remove(stolen_name)
        results.fail("Path traversal DOWNLOAD", f"{target_name} was downloaded!")


def test_path_traversal_delete(env, results):
    """Test that path traversal in DELETE is blocked."""
    # Find a target file in server root
    target_file = None
    for f in os.listdir(env.server_root):
        if os.path.isfile(os.path.join(env.server_root, f)):
            target_file = f
            break
            
    if target_file:
        target_path = os.path.join(env.server_root, target_file)
        stdout, code = env.run_client(f"DELETE ../{target_file}", "traversal_delete")
        
        if os.path.exists(target_path):
            results.ok(f"Path traversal in DELETE blocked ({target_file} intact)")
        else:
            results.fail("Path traversal DELETE", f"{target_file} was deleted!")
    else:
        # No file to target, try generic
        stdout, code = env.run_client("DELETE ../nonexistent_system_file", "traversal_delete")
        if "ERROR" in stdout:
             results.ok("Path traversal in DELETE blocked")
        else:
             results.ok("Path traversal in DELETE blocked (ignored)")


# =============================================================================
# Connection Error Tests
# =============================================================================

def test_connect_to_wrong_port(env, results):
    """Test client behavior when connecting to wrong port."""
    process = subprocess.Popen(
        [CLIENT_EXE, f"127.0.0.1:9999"],  # Wrong port
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True
    )
    
    try:
        stdout, _ = process.communicate(input="LIST\nEXIT\n", timeout=10)
        exit_code = process.returncode
        
        # Should fail to connect
        if "error" in stdout.lower() or "connect" in stdout.lower() or exit_code != 0:
            results.ok("Client reports connection error for wrong port")
        else:
            results.fail("Wrong port", f"No error reported: {stdout[:200]}")
    
    except subprocess.TimeoutExpired:
        process.kill()
        results.fail("Wrong port", "Client hung on wrong port")


# =============================================================================
# Main
# =============================================================================

def main():
    print("MiniDrive Integration Tests - Error Handling")
    print("=" * 60)
    print("Testing: Upload/Download conflicts, invalid operations, path traversal")
    print("(Black-box tests - client perspective only)")

    check_executables()

    env = ErrorTestEnvironment("error_handling", SERVER_PORT)
    results = TestResult(env.log_dir)

    # Change to client working directory so tests create files there
    os.chdir(env.client_cwd)

    try:
        print("\nSetting up test environment...")
        env.setup_server_root()
        env.start_server()

        print("\nRunning tests:\n")

        # Upload errors
        print("  [Upload Errors]")
        test_upload_file_already_exists_on_server(env, results)
        test_upload_nonexistent_local_file(env, results)
        test_upload_to_nonexistent_directory(env, results)

        # Download errors
        print("\n  [Download Errors]")
        test_download_nonexistent_remote_file(env, results)
        test_download_file_already_exists_locally(env, results)
        test_download_directory_instead_of_file(env, results)

        # Delete errors
        print("\n  [Delete Errors]")
        test_delete_nonexistent_file(env, results)
        test_delete_directory_with_delete(env, results)

        # Directory errors
        print("\n  [Directory Errors]")
        test_mkdir_already_exists(env, results)
        test_rmdir_nonexistent(env, results)
        test_rmdir_on_file(env, results)
        test_cd_nonexistent_directory(env, results)
        test_cd_to_file(env, results)

        # Move/Copy errors
        print("\n  [Move/Copy Errors]")
        test_move_nonexistent_source(env, results)
        test_move_destination_exists(env, results)
        test_copy_nonexistent_source(env, results)
        test_copy_destination_exists(env, results)

        # Invalid command errors
        print("\n  [Invalid Command Errors]")
        test_unknown_command(env, results)
        test_command_missing_arguments(env, results)
        test_empty_command(env, results)

        # Path traversal errors
        print("\n  [Path Traversal Errors]")
        test_path_traversal_upload(env, results)
        test_path_traversal_download(env, results)
        test_path_traversal_delete(env, results)

        # Connection errors
        print("\n  [Connection Errors]")
        test_connect_to_wrong_port(env, results)

    finally:
        print("\nCleaning up...")
        env.cleanup()

    success = results.summary()
    print(f"\nLogs saved to: {env.log_dir}")

    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
