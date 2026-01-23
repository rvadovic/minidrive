#!/usr/bin/env python3
"""
Integration tests for MiniDrive Core File Commands.

Tests the following criteria (5 points):
- LIST: List files in directory
- UPLOAD: Upload file to server
- DOWNLOAD: Download file from server  
- DELETE: Delete file on server
- Server handles at least one client simultaneously

Usage:
    python3 tests/integration/test_core_commands.py
"""

import os
import sys

from test_utils import (
    has_ok_response,
    TestEnvironment, TestResult, check_executables, calculate_hash
)

SERVER_PORT = 9020


def test_list_empty(env, results):
    """Test LIST on empty public directory."""
    stdout, code = env.run_client("LIST", "list_empty")
    
    if has_ok_response(stdout):
        results.ok("LIST empty directory")
    else:
        results.fail("LIST empty directory", f"Expected OK in output")


def test_list_with_files(env, results):
    """Test LIST shows files that exist on server."""
    # Create files via UPLOAD
    test_files = ["file1.txt", "file2.txt", "document.pdf"]
    for fname in test_files:
        fname_path = os.path.join(env.client_cwd, fname)
        with open(fname_path, "wb") as f:
            f.write(b"test content\n")
        env.run_client(f"UPLOAD {fname}", f"setup_{fname}")
        os.remove(fname_path)
    
    stdout, code = env.run_client("LIST", "list_with_files")
    
    found_all = all(fname in stdout for fname in test_files)
    if has_ok_response(stdout) and found_all:
        results.ok("LIST shows existing files")
    else:
        results.fail("LIST shows existing files", f"Missing files in output")
    
    # Cleanup is handled by env.cleanup() or individual DELETEs if needed
    # For this test, we leave them (cleaned up at end of suite)


def test_upload_small_file(env, results):
    """Test UPLOAD a small text file."""
    test_file = "upload_small.txt"
    test_file_path = os.path.join(env.client_cwd, test_file)
    test_content = b"Hello, MiniDrive! This is a small test file.\n"
    
    with open(test_file_path, "wb") as f:
        f.write(test_content)
    
    try:
        stdout, code = env.run_client(f"UPLOAD {test_file}", "upload_small")
        
        if not has_ok_response(stdout):
            results.fail("UPLOAD small file", "Upload command failed")
            return
        
        results.ok("UPLOAD small file")
        
        # Verify content via DOWNLOAD
        check_file = "check_upload_small.txt"
        check_file_path = os.path.join(env.client_cwd, check_file)
        env.run_client(f"DOWNLOAD {test_file} {check_file}", "verify_upload_small")
        
        if os.path.exists(check_file_path):
            with open(check_file_path, "rb") as f:
                downloaded_content = f.read()
            if downloaded_content == test_content:
                results.ok("UPLOAD content integrity")
            else:
                results.fail("UPLOAD content integrity", "Content mismatch")
            os.remove(check_file_path)
        else:
            results.fail("UPLOAD content integrity", "Could not download file to verify")
    
    finally:
        if os.path.exists(test_file_path):
            os.remove(test_file_path)


def test_upload_binary_file(env, results):
    """Test UPLOAD a binary file with random content."""
    test_file = "upload_binary.bin"
    test_file_path = os.path.join(env.client_cwd, test_file)
    test_content = os.urandom(1024 * 100)  # 100KB random data
    
    with open(test_file_path, "wb") as f:
        f.write(test_content)
    
    original_hash = calculate_hash(test_file_path)
    
    try:
        stdout, code = env.run_client(f"UPLOAD {test_file}", "upload_binary")
        
        if not has_ok_response(stdout):
            results.fail("UPLOAD binary file", "Upload command failed")
            return
        
        results.ok("UPLOAD binary file")
        
        # Verify hash via DOWNLOAD
        check_file = "check_upload_binary.bin"
        check_file_path = os.path.join(env.client_cwd, check_file)
        env.run_client(f"DOWNLOAD {test_file} {check_file}", "verify_upload_binary")
        
        if os.path.exists(check_file_path):
            downloaded_hash = calculate_hash(check_file_path)
            if downloaded_hash == original_hash:
                results.ok("UPLOAD binary hash matches")
            else:
                results.fail("UPLOAD binary hash matches", "Hash mismatch")
            os.remove(check_file_path)
        else:
            results.fail("UPLOAD binary hash matches", "Could not download file to verify")
    
    finally:
        if os.path.exists(test_file_path):
            os.remove(test_file_path)


def test_upload_large_file(env, results):
    """Test UPLOAD a large file (5MB)."""
    test_file = "upload_large.bin"
    test_file_path = os.path.join(env.client_cwd, test_file)
    file_size = 5 * 1024 * 1024  # 5MB
    
    test_content = os.urandom(file_size)
    with open(test_file_path, "wb") as f:
        f.write(test_content)
    
    original_hash = calculate_hash(test_file_path)
    
    try:
        stdout, code = env.run_client(f"UPLOAD {test_file}", "upload_large", timeout=30)
        
        if not has_ok_response(stdout):
            results.fail("UPLOAD large file (5MB)", "Upload command failed")
            return
        
        results.ok("UPLOAD large file (5MB)")
        
        # Verify hash via DOWNLOAD
        check_file = "check_upload_large.bin"
        check_file_path = os.path.join(env.client_cwd, check_file)
        env.run_client(f"DOWNLOAD {test_file} {check_file}", "verify_upload_large", timeout=30)
        
        if os.path.exists(check_file_path):
            downloaded_hash = calculate_hash(check_file_path)
            if downloaded_hash == original_hash:
                results.ok("UPLOAD large file hash matches")
            else:
                results.fail("UPLOAD large file hash matches", "Hash mismatch")
            os.remove(check_file_path)
        else:
            results.fail("UPLOAD large file hash matches", "Could not download file to verify")
    
    finally:
        if os.path.exists(test_file_path):
            os.remove(test_file_path)





def test_upload_very_large_file(env, results):
    """Test UPLOAD a very large file (100MB)."""
    test_file = "upload_very_large.bin"
    test_file_path = os.path.join(env.client_cwd, test_file)
    file_size = 100 * 1024 * 1024  # 100MB
    
    print(f"  Creating {file_size // (1024*1024)}MB test file...")
    
    # Write in chunks to avoid memory issues
    with open(test_file_path, "wb") as f:
        chunk_size = 1024 * 1024  # 1MB chunks
        for _ in range(file_size // chunk_size):
            f.write(os.urandom(chunk_size))
    
    original_hash = calculate_hash(test_file_path)
    
    try:
        print(f"  Uploading {file_size // (1024*1024)}MB file...")
        stdout, code = env.run_client(f"UPLOAD {test_file}", "upload_very_large", timeout=60)
        
        if not has_ok_response(stdout):
            results.fail("UPLOAD very large file (100MB)", f"Upload command failed: {stdout[:200]}")
            return
        
        results.ok("UPLOAD very large file (100MB)")
        
        # Verify hash via DOWNLOAD
        check_file = "check_upload_very_large.bin"
        check_file_path = os.path.join(env.client_cwd, check_file)
        env.run_client(f"DOWNLOAD {test_file} {check_file}", "verify_upload_very_large", timeout=60)
        
        if os.path.exists(check_file_path):
            downloaded_hash = calculate_hash(check_file_path)
            if downloaded_hash == original_hash:
                results.ok("UPLOAD very large file hash matches")
            else:
                results.fail("UPLOAD very large file hash matches", "Hash mismatch")
            
            # Check file size
            actual_size = os.path.getsize(check_file_path)
            if actual_size == file_size:
                results.ok("UPLOAD very large file size correct")
            else:
                results.fail("UPLOAD very large file size correct", f"Expected {file_size}, got {actual_size}")
            os.remove(check_file_path)
        else:
            results.fail("UPLOAD very large file hash matches", "Could not download file to verify")
    
    finally:
        if os.path.exists(test_file_path):
            os.remove(test_file_path)
        # Cleanup on server is handled by suite cleanup or we can DELETE
        env.run_client(f"DELETE {test_file}", "cleanup_very_large")


def test_download_very_large_file(env, results):
    """Test DOWNLOAD a very large file (100MB)."""
    test_file = "download_very_large.bin"
    test_file_path = os.path.join(env.client_cwd, test_file)
    local_file = "downloaded_very_large.bin"
    local_file_path = os.path.join(env.client_cwd, local_file)
    file_size = 100 * 1024 * 1024  # 100MB
    
    # Create file via UPLOAD
    print(f"  Creating {file_size // (1024*1024)}MB file locally...")
    with open(test_file_path, "wb") as f:
        chunk_size = 1024 * 1024  # 1MB chunks
        for _ in range(file_size // chunk_size):
            f.write(os.urandom(chunk_size))
    
    original_hash = calculate_hash(test_file_path)
    
    try:
        print(f"  Uploading {file_size // (1024*1024)}MB file...")
        env.run_client(f"UPLOAD {test_file}", "setup_download_very_large", timeout=60)
        
        # Remove local file to ensure we download it
        os.remove(test_file_path)
        
        print(f"  Downloading {file_size // (1024*1024)}MB file...")
        stdout, code = env.run_client(f"DOWNLOAD {test_file} {local_file}", "download_very_large", timeout=60)
        
        if not has_ok_response(stdout):
            results.fail("DOWNLOAD very large file (100MB)", f"Download command failed: {stdout[:200]}")
            return
        
        results.ok("DOWNLOAD very large file (100MB)")
        
        if os.path.exists(local_file_path):
            downloaded_hash = calculate_hash(local_file_path)
            if downloaded_hash == original_hash:
                results.ok("DOWNLOAD very large file hash matches")
            else:
                results.fail("DOWNLOAD very large file hash matches", "Hash mismatch")
            
            # Check file size
            actual_size = os.path.getsize(local_file_path)
            if actual_size == file_size:
                results.ok("DOWNLOAD very large file size correct")
            else:
                results.fail("DOWNLOAD very large file size correct", f"Expected {file_size}, got {actual_size}")
        else:
            results.fail("DOWNLOAD very large file hash matches", "Downloaded file not found")
    
    finally:
        if os.path.exists(local_file_path):
            os.remove(local_file_path)
        if os.path.exists(test_file_path):
            os.remove(test_file_path)
        env.run_client(f"DELETE {test_file}", "cleanup_download_very_large")


def test_upload_implicit_remote_path(env, results):
    """Test UPLOAD without specifying remote path (defaults to local filename)."""
    filename = "implicit_upload.txt"
    file_path = os.path.join(env.client_cwd, filename)
    content = b"Implicit upload test"
    
    with open(file_path, "wb") as f:
        f.write(content)
        
    try:
        # Single argument UPLOAD
        stdout, code = env.run_client(f"UPLOAD {filename}", "upload_implicit")
        
        if has_ok_response(stdout):
            # Verify by downloading to a DIFFERENT name
            check_name = "check_implicit_upload.txt"
            env.run_client(f"DOWNLOAD {filename} {check_name}", "verify_implicit_upload")
            
            check_path = os.path.join(env.client_cwd, check_name)
            if os.path.exists(check_path):
                with open(check_path, "rb") as f:
                    if f.read() == content:
                        results.ok("UPLOAD without remote path (defaults to filename)")
                    else:
                        results.fail("UPLOAD without remote path", "Content mismatch")
                os.remove(check_path)
            else:
                results.fail("UPLOAD without remote path", "Could not verify (download failed)")
        else:
            results.fail("UPLOAD without remote path", "Command failed")
            
    finally:
        if os.path.exists(file_path):
            os.remove(file_path)


def test_upload_with_remote_path(env, results):
    """Test UPLOAD with custom remote path."""
    local_file = "local_name.txt"
    local_file_path = os.path.join(env.client_cwd, local_file)
    remote_name = "remote_name.txt"
    test_content = b"Testing custom remote path\n"
    
    with open(local_file_path, "wb") as f:
        f.write(test_content)
    
    try:
        stdout, code = env.run_client(f"UPLOAD {local_file} {remote_name}", "upload_remote_path")
        
        if has_ok_response(stdout):
            # Verify via DOWNLOAD
            check_file = "check_remote_path.txt"
            check_file_path = os.path.join(env.client_cwd, check_file)
            env.run_client(f"DOWNLOAD {remote_name} {check_file}", "verify_remote_path")
            if os.path.exists(check_file_path):
                results.ok("UPLOAD with custom remote path")
                os.remove(check_file_path)
            else:
                results.fail("UPLOAD with custom remote path", "File not at remote path (could not download)")
        else:
            results.fail("UPLOAD with custom remote path", "Upload failed")
    
    finally:
        if os.path.exists(local_file_path):
            os.remove(local_file_path)


def test_upload_nonexistent_file(env, results):
    """Test UPLOAD fails for non-existent local file."""
    stdout, code = env.run_client("UPLOAD nonexistent_file.xyz", "upload_nonexistent")
    
    if "ERROR" in stdout:
        results.ok("UPLOAD nonexistent file returns ERROR")
    else:
        results.fail("UPLOAD nonexistent file returns ERROR", "Expected ERROR")


def test_download_implicit_local_path(env, results):
    """Test DOWNLOAD without specifying local path (defaults to remote filename)."""
    filename = "implicit_download.txt"
    content = b"Implicit download test content"
    
    # Create on server via UPLOAD
    local_setup = os.path.join(env.client_cwd, filename)
    with open(local_setup, "wb") as f:
        f.write(content)
    env.run_client(f"UPLOAD {filename}", "setup_implicit_dl")
    os.remove(local_setup)
    
    # Download with single argument
    stdout, code = env.run_client(f"DOWNLOAD {filename}", "download_implicit")
    
    if has_ok_response(stdout):
        if os.path.exists(local_setup):
            with open(local_setup, "rb") as f:
                if f.read() == content:
                    results.ok("DOWNLOAD without local path (defaults to filename)")
                else:
                    results.fail("DOWNLOAD without local path", "Content mismatch")
            os.remove(local_setup)
        else:
            results.fail("DOWNLOAD without local path", "File not found in cwd")
    else:
        results.fail("DOWNLOAD without local path", "Command failed")


def test_download_file(env, results):
    """Test DOWNLOAD a file from server."""
    server_file = "download_test.txt"
    server_file_path = os.path.join(env.client_cwd, server_file)
    test_content = b"This file was created on the server for download testing.\n"
    
    # Create via UPLOAD
    with open(server_file_path, "wb") as f:
        f.write(test_content)
    env.run_client(f"UPLOAD {server_file}", "setup_download_file")
    os.remove(server_file_path)
    
    local_file = "downloaded.txt"
    local_file_path = os.path.join(env.client_cwd, local_file)
    
    try:
        stdout, code = env.run_client(f"DOWNLOAD {server_file} {local_file}", "download_file")
        
        if not has_ok_response(stdout):
            results.fail("DOWNLOAD file", "Download command failed")
            return
        
        results.ok("DOWNLOAD file")
        
        if os.path.exists(local_file_path):
            with open(local_file_path, "rb") as f:
                local_content = f.read()
            if local_content == test_content:
                results.ok("DOWNLOAD content matches")
            else:
                results.fail("DOWNLOAD content matches", "Content mismatch")
        else:
            results.fail("DOWNLOAD content matches", "Downloaded file not found")
    
    finally:
        if os.path.exists(local_file_path):
            os.remove(local_file_path)
        env.run_client(f"DELETE {server_file}", "cleanup_download_file")


def test_download_binary_file(env, results):
    """Test DOWNLOAD a binary file with hash verification."""
    server_file = "download_binary.bin"
    server_file_path = os.path.join(env.client_cwd, server_file)
    test_content = os.urandom(1024 * 500)  # 500KB
    
    # Create via UPLOAD
    with open(server_file_path, "wb") as f:
        f.write(test_content)
    original_hash = calculate_hash(server_file_path)
    env.run_client(f"UPLOAD {server_file}", "setup_download_binary")
    os.remove(server_file_path)
    
    local_file = "downloaded_binary.bin"
    local_file_path = os.path.join(env.client_cwd, local_file)
    
    try:
        stdout, code = env.run_client(f"DOWNLOAD {server_file} {local_file}", "download_binary")
        
        if not has_ok_response(stdout):
            results.fail("DOWNLOAD binary file", "Download failed")
            return
        
        results.ok("DOWNLOAD binary file")
        
        if os.path.exists(local_file_path):
            local_hash = calculate_hash(local_file_path)
            if local_hash == original_hash:
                results.ok("DOWNLOAD binary hash matches")
            else:
                results.fail("DOWNLOAD binary hash matches", "Hash mismatch")
        else:
            results.fail("DOWNLOAD binary hash matches", "File not found")
    
    finally:
        if os.path.exists(local_file_path):
            os.remove(local_file_path)
        env.run_client(f"DELETE {server_file}", "cleanup_download_binary")


def test_download_nonexistent(env, results):
    """Test DOWNLOAD fails for non-existent remote file."""
    stdout, code = env.run_client("DOWNLOAD nonexistent.xyz local.txt", "download_nonexistent")
    
    if "ERROR" in stdout:
        results.ok("DOWNLOAD nonexistent file returns ERROR")
    else:
        results.fail("DOWNLOAD nonexistent file returns ERROR", "Expected ERROR")
    
    # Cleanup just in case
    local_path = os.path.join(env.client_cwd, "local.txt")
    if os.path.exists(local_path):
        os.remove(local_path)


def test_delete_file(env, results):
    """Test DELETE a file on server."""
    server_file = "to_delete.txt"
    
    # Create via UPLOAD
    with open(server_file, "wb") as f:
        f.write(b"This file will be deleted.\n")
    
    env.run_client(f"UPLOAD {server_file}", "setup_delete_file")
    os.remove(server_file)
    
    stdout, code = env.run_client(f"DELETE {server_file}", "delete_file")
    
    if not has_ok_response(stdout):
        results.fail("DELETE file", "Delete command failed")
        return
    
    results.ok("DELETE file")
    
    # Verify deletion by trying to DOWNLOAD it (should fail)
    stdout, code = env.run_client(f"DOWNLOAD {server_file} check_delete.txt", "check_delete")
    if "ERROR" in stdout:
        results.ok("DELETE removes file from server (verified via DOWNLOAD failure)")
    else:
        results.fail("DELETE removes file from server", "File still downloadable")
        # Cleanup if it still exists
        check_delete_path = os.path.join(env.client_cwd, "check_delete.txt")
        if os.path.exists(check_delete_path):
            os.remove(check_delete_path)


def test_delete_nonexistent(env, results):
    """Test DELETE fails for non-existent file."""
    stdout, code = env.run_client("DELETE nonexistent_file.xyz", "delete_nonexistent")
    
    if "ERROR" in stdout:
        results.ok("DELETE nonexistent file returns ERROR")
    else:
        results.fail("DELETE nonexistent file returns ERROR", "Expected ERROR")


def test_upload_download_roundtrip(env, results):
    """Test full upload then download roundtrip with hash verification."""
    test_file = "roundtrip.bin"
    test_file_path = os.path.join(env.client_cwd, test_file)
    test_content = os.urandom(1024 * 200)  # 200KB
    
    with open(test_file_path, "wb") as f:
        f.write(test_content)
    
    original_hash = calculate_hash(test_file_path)
    downloaded_file = "roundtrip_downloaded.bin"
    downloaded_file_path = os.path.join(env.client_cwd, downloaded_file)
    
    try:
        # Upload
        stdout, code = env.run_client(f"UPLOAD {test_file}", "roundtrip_upload")
        if not has_ok_response(stdout):
            results.fail("Roundtrip upload", "Upload failed")
            return
        
        # Remove local file
        os.remove(test_file_path)
        
        # Download
        stdout, code = env.run_client(f"DOWNLOAD {test_file} {downloaded_file}", "roundtrip_download")
        if not has_ok_response(stdout):
            results.fail("Roundtrip download", "Download failed")
            return
        
        # Verify
        if os.path.exists(downloaded_file_path):
            downloaded_hash = calculate_hash(downloaded_file_path)
            if downloaded_hash == original_hash:
                results.ok("Upload/Download roundtrip integrity")
            else:
                results.fail("Upload/Download roundtrip integrity", "Hash mismatch")
        else:
            results.fail("Upload/Download roundtrip integrity", "Downloaded file not found")
    
    finally:
        if os.path.exists(test_file_path):
            os.remove(test_file_path)
        if os.path.exists(downloaded_file_path):
            os.remove(downloaded_file_path)


def _verify_upload_on_server(env, results, test_name, file_size):
    """Helper to upload a file and verify it exists on server disk with correct hash."""
    filename = f"{test_name}.bin"
    file_path = os.path.join(env.client_cwd, filename)
    print(f"  Creating {file_size // 1024}KB test file for {test_name}...")
    
    # Create file
    with open(file_path, "wb") as f:
        # Write in chunks if large
        chunk_size = 1024 * 1024
        remaining = file_size
        while remaining > 0:
            to_write = min(remaining, chunk_size)
            f.write(os.urandom(to_write))
            remaining -= to_write
            
    original_hash = calculate_hash(file_path)
    
    try:
        # Upload
        print(f"  Uploading {filename}...")
        stdout, code = env.run_client(f"UPLOAD {filename}", f"{test_name}_upload", timeout=60)
        if not has_ok_response(stdout):
            results.fail(f"{test_name} upload", "Upload failed")
            return
        
        # Walk server root to find the file
        found_path = None
        for root, dirs, files in os.walk(env.server_root):
            if filename in files:
                found_path = os.path.join(root, filename)
                break
        
        if found_path:
            results.ok(f"{test_name}: File found in server root")
            
            # Verify content hash directly on server file
            server_hash = calculate_hash(found_path)
            if server_hash == original_hash:
                results.ok(f"{test_name}: Server file hash matches")
            else:
                results.fail(f"{test_name}", f"Hash mismatch. Local: {original_hash}, Server: {server_hash}")
        else:
            results.fail(f"{test_name}", "File not found in server root")
            
    finally:
        if os.path.exists(file_path):
            os.remove(file_path)


def test_upload_server_verification_small(env, results):
    """Test UPLOAD (50KB) by directly inspecting server filesystem."""
    _verify_upload_on_server(env, results, "server_verify_small", 50 * 1024)


def test_upload_server_verification_large(env, results):
    """Test UPLOAD (50MB) by directly inspecting server filesystem."""
    _verify_upload_on_server(env, results, "server_verify_large", 50 * 1024 * 1024)


def main():
    print("MiniDrive Integration Tests - Core File Commands")
    print("=" * 60)
    print("Testing: LIST, UPLOAD, DOWNLOAD, DELETE")
    
    check_executables()
    
    env = TestEnvironment("core_commands", SERVER_PORT)
    results = TestResult(env.log_dir)
    
    # Change to client working directory so tests create files there
    os.chdir(env.client_cwd)
    
    try:
        print("\nSetting up test environment...")
        env.setup_server_root()
        env.start_server()
        
        print("\nRunning tests:\n")
        
        # LIST tests
        test_list_empty(env, results)
        test_list_with_files(env, results)
        
        # UPLOAD tests
        test_upload_small_file(env, results)
        test_upload_binary_file(env, results)
        test_upload_large_file(env, results)
        test_upload_very_large_file(env, results)
        test_upload_implicit_remote_path(env, results)
        test_upload_with_remote_path(env, results)
        test_upload_nonexistent_file(env, results)
        
        # DOWNLOAD tests
        test_download_implicit_local_path(env, results)
        test_download_file(env, results)
        test_download_binary_file(env, results)
        test_download_very_large_file(env, results)
        test_download_nonexistent(env, results)
        
        # DELETE tests
        test_delete_file(env, results)
        test_delete_nonexistent(env, results)
        
        # Roundtrip test
        test_upload_download_roundtrip(env, results)
        
        # Server verification tests (White-box)
        test_upload_server_verification_small(env, results)
        test_upload_server_verification_large(env, results)
        
    finally:
        print("\nCleaning up...")
        env.cleanup()
    
    success = results.summary()
    print(f"\nLogs saved to: {env.log_dir}")
    
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
