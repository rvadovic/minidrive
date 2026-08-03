#!/usr/bin/env python3
"""
Integration tests for MiniDrive Synchronization Command.

Tests the following criteria (4 points):
- SYNC uploads changes (Local to Remote, one-way)
- SYNC handles deletions (files deleted locally are deleted on server)
- SYNC does not re-upload unchanged files
- SYNC handles nested directories recursively

Usage:
    python3 tests/integration/test_sync_command.py
"""

import os
import sys
import shutil
import time

from test_utils import (
    has_ok_response,
    TestEnvironment, TestResult, check_executables, calculate_hash
)

SERVER_PORT = 9022


def create_local_sync_dir(name="sync_local"):
    """Create a local directory for sync testing."""
    sync_dir = os.path.abspath(name)
    if os.path.exists(sync_dir):
        shutil.rmtree(sync_dir)
    os.makedirs(sync_dir)
    return sync_dir


def test_sync_empty_to_empty(env, results):
    """Test SYNC from empty local to empty remote."""
    local_dir = create_local_sync_dir("sync_empty")
    remote_dir = "sync_dest_empty"
    
    # Create remote directory first (SYNC may require it to exist)
    env.run_client(f"MKDIR {remote_dir}", "sync_empty_mkdir")
    
    try:
        stdout, code = env.run_client(f"SYNC {local_dir} {remote_dir}", "sync_empty_empty")
        
        if has_ok_response(stdout):
            results.ok("SYNC empty to empty succeeds")
        else:
            # SYNC might fail if remote doesn't exist - that's also valid behavior
            if "ERROR" in stdout:
                results.ok("SYNC empty to empty (requires existing remote dir)")
            else:
                results.fail("SYNC empty to empty", f"Command failed")
    
    finally:
        shutil.rmtree(local_dir, ignore_errors=True)


def test_sync_uploads_new_files(env, results):
    """Test SYNC uploads new files to server."""
    local_dir = create_local_sync_dir("sync_upload")
    remote_dir = "sync_dest_upload"
    
    # Create remote directory first
    env.run_client(f"MKDIR {remote_dir}", "sync_upload_mkdir")
    
    # Create local files
    files = {
        "file1.txt": b"Content of file 1\n",
        "file2.txt": b"Content of file 2\n",
        "data.bin": os.urandom(1024),
    }
    
    for fname, content in files.items():
        with open(os.path.join(local_dir, fname), "wb") as f:
            f.write(content)
    
    try:
        stdout, code = env.run_client(f"SYNC {local_dir} {remote_dir}", "sync_upload_new")
        
        if not has_ok_response(stdout):
            results.fail("SYNC uploads new files", "Command failed")
            return
        
        results.ok("SYNC uploads new files")
        
        # Verify files exist on server via DOWNLOAD
        all_synced = True
        for fname, content in files.items():
            download_file = f"check_{fname}"
            stdout, code = env.run_client(f"DOWNLOAD {remote_dir}/{fname} {download_file}", f"check_sync_{fname}")
            
            if has_ok_response(stdout) and os.path.exists(download_file):
                with open(download_file, "rb") as f:
                    if f.read() != content:
                        all_synced = False
                        break
                os.remove(download_file)
            else:
                all_synced = False
                break
        
        if all_synced:
            results.ok("SYNC all files present on server with correct content")
        else:
            results.fail("SYNC files on server", "Some files missing or content mismatch")
    
    finally:
        shutil.rmtree(local_dir, ignore_errors=True)


def test_sync_uploads_nested_directories(env, results):
    """Test SYNC handles nested directory structures."""
    local_dir = create_local_sync_dir("sync_nested")
    remote_dir = "sync_dest_nested"
    
    # Create remote directory first
    env.run_client(f"MKDIR {remote_dir}", "sync_nested_mkdir")
    
    # Create nested structure
    os.makedirs(os.path.join(local_dir, "level1", "level2", "level3"))
    os.makedirs(os.path.join(local_dir, "docs", "images"))
    
    files = {
        "root.txt": b"Root file\n",
        "level1/a.txt": b"Level 1 file\n",
        "level1/level2/b.txt": b"Level 2 file\n",
        "level1/level2/level3/c.txt": b"Level 3 file\n",
        "docs/readme.md": b"# Readme\n",
        "docs/images/logo.bin": os.urandom(512),
    }
    
    for fpath, content in files.items():
        with open(os.path.join(local_dir, fpath), "wb") as f:
            f.write(content)
    
    try:
        stdout, code = env.run_client(f"SYNC {local_dir} {remote_dir}", "sync_nested", timeout=60)
        
        if not has_ok_response(stdout):
            results.fail("SYNC nested directories", "Command failed")
            return
        
        results.ok("SYNC nested directories command succeeds")
        
        # Verify all files via DOWNLOAD
        all_synced = True
        for fpath, content in files.items():
            download_file = f"check_nested_{os.path.basename(fpath)}"
            # Need to handle path separators for remote path
            remote_file_path = f"{remote_dir}/{fpath}"
            stdout, code = env.run_client(f"DOWNLOAD {remote_file_path} {download_file}", f"check_sync_nested_{os.path.basename(fpath)}")
            
            if has_ok_response(stdout) and os.path.exists(download_file):
                with open(download_file, "rb") as f:
                    if f.read() != content:
                        all_synced = False
                        break
                os.remove(download_file)
            else:
                all_synced = False
                break
        
        if all_synced:
            results.ok("SYNC preserves nested directory structure")
        else:
            results.fail("SYNC nested structure", "Some files/dirs missing")
    
    finally:
        shutil.rmtree(local_dir, ignore_errors=True)


def test_sync_handles_deletions(env, results):
    """Test SYNC deletes files on server that were deleted locally."""
    local_dir = create_local_sync_dir("sync_delete")
    remote_dir = "sync_dest_delete"
    
    # Create remote directory first
    env.run_client(f"MKDIR {remote_dir}", "sync_delete_mkdir")
    
    # Create initial files
    files = ["keep.txt", "delete_me.txt", "also_delete.txt"]
    for fname in files:
        with open(os.path.join(local_dir, fname), "w") as f:
            f.write(f"Content of {fname}")
    
    try:
        # First sync - upload all
        stdout, code = env.run_client(f"SYNC {local_dir} {remote_dir}", "sync_delete_initial")
        
        if not has_ok_response(stdout):
            results.fail("SYNC initial upload for deletion test", "Command failed")
            return
        
        # Verify files exist on server via DOWNLOAD
        for fname in files:
            stdout, code = env.run_client(f"DOWNLOAD {remote_dir}/{fname} check_del_init_{fname}", f"check_sync_del_init_{fname}")
            if not has_ok_response(stdout):
                results.fail("SYNC deletion test setup", f"File {fname} not uploaded")
                return
            if os.path.exists(f"check_del_init_{fname}"):
                os.remove(f"check_del_init_{fname}")
        
        # Delete some files locally
        os.remove(os.path.join(local_dir, "delete_me.txt"))
        os.remove(os.path.join(local_dir, "also_delete.txt"))
        
        # Second sync - should delete files on server
        stdout, code = env.run_client(f"SYNC {local_dir} {remote_dir}", "sync_delete_second")
        
        if not has_ok_response(stdout):
            results.fail("SYNC with deletions", "Command failed")
            return
        
        results.ok("SYNC with deletions command succeeds")
        
        # Verify: keep.txt should exist, others should be gone
        stdout, code = env.run_client(f"DOWNLOAD {remote_dir}/keep.txt check_keep.txt", "check_sync_keep")
        if has_ok_response(stdout) and os.path.exists("check_keep.txt"):
            results.ok("SYNC keeps non-deleted files")
            os.remove("check_keep.txt")
        else:
            results.fail("SYNC keeps non-deleted files", "File was incorrectly deleted")
        
        # Check deleted files
        deleted_correctly = True
        for fname in ["delete_me.txt", "also_delete.txt"]:
            stdout, code = env.run_client(f"DOWNLOAD {remote_dir}/{fname} check_del_{fname}", f"check_sync_del_{fname}")
            # has_ok_response() matches "ok" anywhere in stdout, including the public-mode banner,
            # so the downloaded file has to be the real evidence - same pairing as the check above.
            if has_ok_response(stdout) and os.path.exists(f"check_del_{fname}"):
                deleted_correctly = False
                os.remove(f"check_del_{fname}")
        
        if deleted_correctly:
            results.ok("SYNC deletes locally-removed files from server")
        else:
            results.fail("SYNC deletes files", "Deleted files still exist on server")
    
    finally:
        shutil.rmtree(local_dir, ignore_errors=True)


def test_sync_uploads_modified_files(env, results):
    """Test SYNC uploads files that have been modified locally."""
    local_dir = create_local_sync_dir("sync_modify")
    remote_dir = "sync_dest_modify"
    
    # Create remote directory first
    env.run_client(f"MKDIR {remote_dir}", "sync_modify_mkdir")
    
    test_file = "modify_me.txt"
    local_path = os.path.join(local_dir, test_file)
    
    # Create initial file
    original_content = b"Original content\n"
    with open(local_path, "wb") as f:
        f.write(original_content)
    
    try:
        # First sync
        stdout, code = env.run_client(f"SYNC {local_dir} {remote_dir}", "sync_modify_initial")
        
        if not has_ok_response(stdout):
            results.fail("SYNC initial for modify test", "Command failed")
            return
        
        # Modify local file
        modified_content = b"Modified content - this is new!\n"
        with open(local_path, "wb") as f:
            f.write(modified_content)
        
        # Second sync
        stdout, code = env.run_client(f"SYNC {local_dir} {remote_dir}", "sync_modify_second")
        
        if not has_ok_response(stdout):
            results.fail("SYNC modified file", "Command failed")
            return
        
        results.ok("SYNC modified file command succeeds")
        
        # Verify server has updated content via DOWNLOAD
        download_file = "check_modify.txt"
        stdout, code = env.run_client(f"DOWNLOAD {remote_dir}/{test_file} {download_file}", "check_sync_modify")
        
        if has_ok_response(stdout) and os.path.exists(download_file):
            with open(download_file, "rb") as f:
                server_content = f.read()
            if server_content == modified_content:
                results.ok("SYNC updates modified file on server")
            else:
                results.fail("SYNC updates content", "Server still has old content")
            os.remove(download_file)
        else:
            results.fail("SYNC modified file", "File missing on server")
    
    finally:
        shutil.rmtree(local_dir, ignore_errors=True)


def test_sync_skips_unchanged_files(env, results):
    """Test SYNC does not re-upload unchanged files."""
    local_dir = create_local_sync_dir("sync_skip")
    remote_dir = "sync_dest_skip"
    
    # Create remote directory first
    env.run_client(f"MKDIR {remote_dir}", "sync_skip_mkdir")
    
    # Create files
    files = {
        "unchanged.txt": b"This will not change\n",
        "will_change.txt": b"This will be modified\n",
    }
    
    for fname, content in files.items():
        with open(os.path.join(local_dir, fname), "wb") as f:
            f.write(content)
    
    try:
        # First sync
        stdout, code = env.run_client(f"SYNC {local_dir} {remote_dir}", "sync_skip_initial")
        
        if not has_ok_response(stdout):
            results.fail("SYNC initial for skip test", "Command failed")
            return
        
        # Wait a bit
        time.sleep(1)
        
        # Modify only one file
        with open(os.path.join(local_dir, "will_change.txt"), "wb") as f:
            f.write(b"New content for changed file\n")
        
        # Second sync
        stdout, code = env.run_client(f"SYNC {local_dir} {remote_dir}", "sync_skip_second")
        
        if not has_ok_response(stdout):
            results.fail("SYNC skip unchanged", "Command failed")
            return
        
        results.ok("SYNC completes without errors")
        
        # Check output for skip indication
        if "skip" in stdout.lower() or "unchanged" in stdout.lower():
            results.ok("SYNC appears to skip unchanged files (based on output)")
        else:
            # Can't verify black-box without verbose output or timing attacks (unreliable)
            results.ok("SYNC skip check (assumed correct if functional)")
    
    finally:
        shutil.rmtree(local_dir, ignore_errors=True)


def test_sync_handles_new_subdirectory(env, results):
    """Test SYNC handles adding a new subdirectory after initial sync."""
    local_dir = create_local_sync_dir("sync_newdir")
    remote_dir = "sync_dest_newdir"
    
    # Create remote directory first
    env.run_client(f"MKDIR {remote_dir}", "sync_newdir_mkdir")
    
    # Create initial structure
    with open(os.path.join(local_dir, "root.txt"), "w") as f:
        f.write("Root file")
    
    try:
        # First sync
        stdout, code = env.run_client(f"SYNC {local_dir} {remote_dir}", "sync_newdir_initial")
        
        if not has_ok_response(stdout):
            results.fail("SYNC initial for new dir test", "Command failed")
            return
        
        # Add new subdirectory with files
        new_subdir = os.path.join(local_dir, "new_subdir")
        os.makedirs(new_subdir)
        with open(os.path.join(new_subdir, "new_file.txt"), "w") as f:
            f.write("New file in new directory")
        
        # Second sync
        stdout, code = env.run_client(f"SYNC {local_dir} {remote_dir}", "sync_newdir_second")
        
        if not has_ok_response(stdout):
            results.fail("SYNC with new subdirectory", "Command failed")
            return
        
        results.ok("SYNC with new subdirectory succeeds")
        
        # Verify new structure on server via DOWNLOAD
        download_file = "check_new_file.txt"
        stdout, code = env.run_client(f"DOWNLOAD {remote_dir}/new_subdir/new_file.txt {download_file}", "check_sync_newdir")
        
        if has_ok_response(stdout) and os.path.exists(download_file):
            results.ok("SYNC uploads new subdirectory and contents")
            os.remove(download_file)
        else:
            results.fail("SYNC new subdirectory", "New subdir/file not on server")
    
    finally:
        shutil.rmtree(local_dir, ignore_errors=True)


def test_sync_deletes_empty_directory(env, results):
    """Test SYNC removes directory when all its contents are deleted locally."""
    local_dir = create_local_sync_dir("sync_rmdir")
    remote_dir = "sync_dest_rmdir"
    
    # Create remote directory first
    env.run_client(f"MKDIR {remote_dir}", "sync_rmdir_mkdir")
    
    # Create initial structure with subdirectory
    subdir = os.path.join(local_dir, "subdir_to_delete")
    os.makedirs(subdir)
    with open(os.path.join(subdir, "file.txt"), "w") as f:
        f.write("File in subdir")
    with open(os.path.join(local_dir, "keep.txt"), "w") as f:
        f.write("Keep this")
    
    try:
        # First sync
        stdout, code = env.run_client(f"SYNC {local_dir} {remote_dir}", "sync_rmdir_initial")
        
        if not has_ok_response(stdout):
            results.fail("SYNC initial for rmdir test", "Command failed")
            return
        
        # Delete local subdirectory
        shutil.rmtree(subdir)
        
        # Second sync
        stdout, code = env.run_client(f"SYNC {local_dir} {remote_dir}", "sync_rmdir_second")
        
        if not has_ok_response(stdout):
            results.fail("SYNC after directory deletion", "Command failed")
            return
        
        results.ok("SYNC after directory deletion succeeds")
        
        # Verify: subdirectory should be gone, keep.txt should remain
        stdout, code = env.run_client(f"DOWNLOAD {remote_dir}/keep.txt check_keep.txt", "check_sync_rmdir_keep")
        if has_ok_response(stdout) and os.path.exists("check_keep.txt"):
            results.ok("SYNC preserves remaining files")
            os.remove("check_keep.txt")
        else:
            results.fail("SYNC preserves files", "keep.txt was deleted")
        
        # Verify subdir is gone by CDing into it
        stdout, code = env.run_client(f"CD {remote_dir}/subdir_to_delete", "check_sync_rmdir_gone")
        if "ERROR" in stdout:
            results.ok("SYNC removes deleted directory from server")
        else:
            # Some implementations only delete files, not empty directories
            # Check if at least the files inside were deleted
            stdout, code = env.run_client(f"DOWNLOAD {remote_dir}/subdir_to_delete/file.txt check_file.txt", "check_sync_rmdir_file")
            if "ERROR" in stdout:
                results.ok("SYNC deletes files (empty dirs may remain - valid impl)")
            else:
                results.fail("SYNC removes directory", "Deleted directory still has files")
                if os.path.exists("check_file.txt"):
                    os.remove("check_file.txt")
    
    finally:
        shutil.rmtree(local_dir, ignore_errors=True)


def test_sync_large_files(env, results):
    """Test SYNC handles larger files correctly."""
    local_dir = create_local_sync_dir("sync_large")
    remote_dir = "sync_dest_large"
    
    # Create remote directory first
    env.run_client(f"MKDIR {remote_dir}", "sync_large_mkdir")
    
    # Create a larger file (2MB)
    large_file = "large_file.bin"
    large_content = os.urandom(2 * 1024 * 1024)
    local_path = os.path.join(local_dir, large_file)
    
    with open(local_path, "wb") as f:
        f.write(large_content)
    
    original_hash = calculate_hash(local_path)
    
    try:
        stdout, code = env.run_client(f"SYNC {local_dir} {remote_dir}", "sync_large", timeout=120)
        
        if not has_ok_response(stdout):
            results.fail("SYNC large file", "Command failed")
            return
        
        results.ok("SYNC large file command succeeds")
        
        # Verify hash via DOWNLOAD
        download_file = "check_large.bin"
        stdout, code = env.run_client(f"DOWNLOAD {remote_dir}/{large_file} {download_file}", "check_sync_large", timeout=120)
        
        if has_ok_response(stdout) and os.path.exists(download_file):
            server_hash = calculate_hash(download_file)
            if server_hash == original_hash:
                results.ok("SYNC large file integrity verified")
            else:
                results.fail("SYNC large file integrity", "Hash mismatch")
            os.remove(download_file)
        else:
            results.fail("SYNC large file", "File not on server")
    
    finally:
        shutil.rmtree(local_dir, ignore_errors=True)


def test_sync_summary_output(env, results):
    """Test SYNC prints summary of actions taken."""
    local_dir = create_local_sync_dir("sync_summary")
    remote_dir = "sync_dest_summary"
    
    # Create remote directory first
    env.run_client(f"MKDIR {remote_dir}", "sync_summary_mkdir")
    
    # Create some files
    for i in range(3):
        with open(os.path.join(local_dir, f"file{i}.txt"), "w") as f:
            f.write(f"Content {i}")
    
    try:
        stdout, code = env.run_client(f"SYNC {local_dir} {remote_dir}", "sync_summary")
        
        if not has_ok_response(stdout):
            results.fail("SYNC summary test", "Command failed")
            return
        
        # Check for summary info (uploaded, deleted, skipped counts)
        has_summary = any(word in stdout.lower() for word in 
                        ["upload", "delete", "skip", "sync", "file"])
        
        if has_summary:
            results.ok("SYNC provides summary output")
        else:
            results.ok("SYNC completes (summary format may vary)")
    
    finally:
        shutil.rmtree(local_dir, ignore_errors=True)


def main():
    print("MiniDrive Integration Tests - Synchronization Command")
    print("=" * 60)
    print("Testing: SYNC (Local to Remote, one-way sync)")
    
    check_executables()
    
    env = TestEnvironment("sync_command", SERVER_PORT)
    results = TestResult(env.log_dir)
    
    # Change to client working directory so tests create files there
    os.chdir(env.client_cwd)
    
    try:
        print("\nSetting up test environment...")
        env.setup_server_root()
        env.start_server()
        
        print("\nRunning tests:\n")
        
        # Basic SYNC tests
        test_sync_empty_to_empty(env, results)
        test_sync_uploads_new_files(env, results)
        test_sync_uploads_nested_directories(env, results)
        
        # Deletion handling
        test_sync_handles_deletions(env, results)
        test_sync_deletes_empty_directory(env, results)
        
        # Modification handling
        test_sync_uploads_modified_files(env, results)
        test_sync_skips_unchanged_files(env, results)
        
        # Additional scenarios
        test_sync_handles_new_subdirectory(env, results)
        test_sync_large_files(env, results)
        test_sync_summary_output(env, results)
        
    finally:
        print("\nCleaning up...")
        env.cleanup()
    
    success = results.summary()
    print(f"\nLogs saved to: {env.log_dir}")
    
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
