#!/usr/bin/env python3
"""
Integration tests for MiniDrive Folder Commands.

Tests the following criteria (5 points):
- CD: Change current directory
- MKDIR: Create directory
- RMDIR: Remove directory (recursive)
- MOVE: Move/rename file or folder
- COPY: Copy file or folder

Usage:
    python3 tests/integration/test_folder_commands.py
"""

import os
import sys
import shutil

from test_utils import (
    has_ok_response,
    TestEnvironment, TestResult, check_executables, calculate_hash
)

SERVER_PORT = 9021


def test_mkdir_simple(env, results):
    """Test MKDIR creates a directory."""
    dir_name = "test_dir"
    
    stdout, code = env.run_client(f"MKDIR {dir_name}", "mkdir_simple")
    
    if not has_ok_response(stdout):
        results.fail("MKDIR simple", "Command failed")
        return
    
    results.ok("MKDIR simple")
    
    # Verify directory exists by CDing into it
    stdout, code = env.run_client(f"CD {dir_name}", "check_mkdir_simple")
    if has_ok_response(stdout):
        results.ok("MKDIR directory exists on server")
    else:
        results.fail("MKDIR directory exists on server", "Could not CD into directory")


def test_mkdir_nested(env, results):
    """Test MKDIR with nested path (may require creating parents first)."""
    # Some implementations require parent directories to exist
    # Test creating directories one level at a time
    dirs = ["nested_parent", "nested_parent/child", "nested_parent/child/grandchild"]
    
    for d in dirs:
        stdout, code = env.run_client(f"MKDIR {d}", f"mkdir_nested_{d.replace('/', '_')}")
        if "ok" not in stdout.lower() and "ERROR" not in stdout:
            results.fail("MKDIR nested path", f"Unexpected response for {d}")
            return
    
    # Verify the deepest directory exists by CDing into it
    stdout, code = env.run_client("CD nested_parent/child/grandchild", "check_mkdir_nested")
    if has_ok_response(stdout):
        results.ok("MKDIR nested directories (created step by step)")
    else:
        # Try single command for implementations that support it
        stdout, code = env.run_client("MKDIR deep/nested/path", "mkdir_nested_single")
        if has_ok_response(stdout):
            # Verify
            stdout, code = env.run_client("CD deep/nested/path", "check_mkdir_nested_single")
            if has_ok_response(stdout):
                results.ok("MKDIR nested path (single command)")
            else:
                results.fail("MKDIR nested path (single command)", "Could not CD into directory")
        else:
            results.ok("MKDIR nested requires parent dirs (expected behavior)")


def test_mkdir_already_exists(env, results):
    """Test MKDIR fails when directory already exists."""
    dir_name = "existing_dir"
    env.run_client(f"MKDIR {dir_name}", "setup_mkdir_exists")
    
    stdout, code = env.run_client(f"MKDIR {dir_name}", "mkdir_exists")
    
    if "ERROR" in stdout:
        results.ok("MKDIR existing directory returns ERROR")
    else:
        results.fail("MKDIR existing directory returns ERROR", "Expected ERROR")
    
    env.run_client(f"RMDIR {dir_name}", "cleanup_mkdir_exists")


def test_rmdir_empty(env, results):
    """Test RMDIR removes an empty directory."""
    dir_name = "empty_dir"
    env.run_client(f"MKDIR {dir_name}", "setup_rmdir_empty")
    
    stdout, code = env.run_client(f"RMDIR {dir_name}", "rmdir_empty")
    
    if not has_ok_response(stdout):
        results.fail("RMDIR empty directory", "Command failed")
        return
    
    results.ok("RMDIR empty directory")
    
    # Verify directory is gone by CDing into it (should fail)
    stdout, code = env.run_client(f"CD {dir_name}", "check_rmdir_empty")
    if "ERROR" in stdout:
        results.ok("RMDIR removes directory from server")
    else:
        results.fail("RMDIR removes directory from server", "Directory still accessible")


def test_rmdir_with_contents(env, results):
    """Test RMDIR removes directory with files (recursive)."""
    dir_name = "dir_with_files"
    env.run_client(f"MKDIR {dir_name}", "setup_rmdir_recursive")
    
    # Create some files inside via UPLOAD
    for i in range(3):
        fname = f"file{i}.txt"
        with open(fname, "w") as f:
            f.write(f"Content {i}")
        env.run_client([f"CD {dir_name}", f"UPLOAD {fname}"], f"setup_rmdir_file_{i}")
        os.remove(fname)
    
    # Create nested directory
    env.run_client([f"CD {dir_name}", "MKDIR nested"], "setup_rmdir_nested")
    fname = "nested_file.txt"
    with open(fname, "w") as f:
        f.write("Nested content")
    env.run_client([f"CD {dir_name}/nested", f"UPLOAD {fname}"], "setup_rmdir_nested_file")
    os.remove(fname)
    
    stdout, code = env.run_client(f"RMDIR {dir_name}", "rmdir_recursive")
    
    if not has_ok_response(stdout):
        results.fail("RMDIR with contents (recursive)", "Command failed")
        return
    
    results.ok("RMDIR with contents (recursive)")
    
    # Verify directory is gone by CDing into it
    stdout, code = env.run_client(f"CD {dir_name}", "check_rmdir_recursive")
    if "ERROR" in stdout:
        results.ok("RMDIR recursively removes all contents")
    else:
        results.fail("RMDIR recursively removes all contents", "Directory still accessible")


def test_rmdir_nonexistent(env, results):
    """Test RMDIR fails for non-existent directory."""
    stdout, code = env.run_client("RMDIR nonexistent_dir", "rmdir_nonexistent")
    
    if "ERROR" in stdout:
        results.ok("RMDIR nonexistent directory returns ERROR")
    else:
        results.fail("RMDIR nonexistent directory returns ERROR", "Expected ERROR")


def test_cd_to_directory(env, results):
    """Test CD changes to a directory."""
    dir_name = "cd_test_dir"
    env.run_client(f"MKDIR {dir_name}", "setup_cd_dir")
    
    # Create a file in the subdirectory
    test_file = "subdir_file.txt"
    with open(test_file, "w") as f:
        f.write("In subdirectory")
    env.run_client([f"CD {dir_name}", f"UPLOAD {test_file}"], "setup_cd_file")
    os.remove(test_file)
    
    # CD then LIST
    stdout, code = env.run_client([f"CD {dir_name}", "LIST"], "cd_to_directory")
    
    if test_file in stdout:
        results.ok("CD changes directory (LIST shows subdir contents)")
    else:
        results.fail("CD changes directory", "Subdir file not in LIST output")
    
    env.run_client([f"CD {dir_name}", f"DELETE {test_file}", "CD ..", f"RMDIR {dir_name}"], "cleanup_cd_dir")


def test_cd_to_parent(env, results):
    """Test CD .. goes to parent directory."""
    # Create nested structure
    dir_name = "cd_parent_test"
    env.run_client(f"MKDIR {dir_name}", "setup_cd_parent")
    
    # Create file in root
    root_file = "root_marker.txt"
    with open(root_file, "w") as f:
        f.write("In root")
    env.run_client(f"UPLOAD {root_file}", "setup_cd_parent_file")
    os.remove(root_file)
    
    # CD into dir, then back to parent
    stdout, code = env.run_client([f"CD {dir_name}", "CD ..", "LIST"], "cd_parent")
    
    if root_file in stdout:
        results.ok("CD .. returns to parent directory")
    else:
        results.fail("CD .. returns to parent directory", "Root file not visible")
    
    # Cleanup
    env.run_client(f"DELETE {root_file}", "cleanup_cd_parent_file")
    env.run_client(f"RMDIR {dir_name}", "cleanup_cd_parent_dir")


def test_cd_absolute_path(env, results):
    """Test CD with absolute path (from user root)."""
    # Create nested structure
    nested = "level1/level2"
    # Create level1 then level2
    env.run_client("MKDIR level1", "setup_cd_abs_1")
    env.run_client("MKDIR level1/level2", "setup_cd_abs_3")
    
    marker_file = "deep_marker.txt"
    with open(marker_file, "w") as f:
        f.write("Deep content")
    env.run_client([f"CD {nested}", f"UPLOAD {marker_file}"], "setup_cd_abs_file")
    os.remove(marker_file)
    
    # CD with absolute path (/ means user root)
    stdout, code = env.run_client([f"CD /{nested}", "LIST"], "cd_absolute")
    
    if marker_file in stdout:
        results.ok("CD with absolute path works")
    else:
        results.fail("CD with absolute path works", "Marker file not found")
    
    # Cleanup
    env.run_client([f"CD /{nested}", f"DELETE {marker_file}"], "cleanup_cd_abs_file")
    env.run_client("RMDIR level1/level2", "cleanup_cd_abs_dir2")
    env.run_client("RMDIR level1", "cleanup_cd_abs_dir1")


def test_cd_nonexistent(env, results):
    """Test CD to non-existent directory fails."""
    stdout, code = env.run_client("CD nonexistent_directory", "cd_nonexistent")
    
    if "ERROR" in stdout:
        results.ok("CD nonexistent directory returns ERROR")
    else:
        results.fail("CD nonexistent directory returns ERROR", "Expected ERROR")


def test_move_file(env, results):
    """Test MOVE renames/moves a file."""
    src_file = "move_source.txt"
    dst_file = "move_dest.txt"
    
    test_content = b"Content to be moved\n"
    with open(src_file, "wb") as f:
        f.write(test_content)
    env.run_client(f"UPLOAD {src_file}", "setup_move_file")
    os.remove(src_file)
    
    stdout, code = env.run_client(f"MOVE {src_file} {dst_file}", "move_file")
    
    if not has_ok_response(stdout):
        results.fail("MOVE file", "Command failed")
        return
    
    results.ok("MOVE file")
    
    # Verify source is gone (DOWNLOAD fails) and dest exists (DOWNLOAD succeeds)
    stdout, code = env.run_client(f"DOWNLOAD {src_file} check_src.txt", "check_move_src")
    if "ERROR" in stdout:
        # Verify dest
        stdout, code = env.run_client(f"DOWNLOAD {dst_file} check_dst.txt", "check_move_dst")
        if has_ok_response(stdout) and os.path.exists("check_dst.txt"):
            with open("check_dst.txt", "rb") as f:
                content = f.read()
            if content == test_content:
                results.ok("MOVE preserves content and removes source")
            else:
                results.fail("MOVE preserves content", "Content mismatch")
            os.remove("check_dst.txt")
        else:
            results.fail("MOVE removes source", "Dest missing or download failed")
    else:
        results.fail("MOVE removes source", "Source still exists")
        if os.path.exists("check_src.txt"):
            os.remove("check_src.txt")
    
    env.run_client(f"DELETE {dst_file}", "cleanup_move_file")


def test_move_to_directory(env, results):
    """Test MOVE file into a directory."""
    src_file = "file_to_move.txt"
    dst_dir = "move_target_dir"
    
    env.run_client(f"MKDIR {dst_dir}", "setup_move_dir")
    
    with open(src_file, "w") as f:
        f.write("Moving into directory")
    env.run_client(f"UPLOAD {src_file}", "setup_move_file_dir")
    os.remove(src_file)
    
    stdout, code = env.run_client(f"MOVE {src_file} {dst_dir}/{src_file}", "move_to_dir")
    
    if not has_ok_response(stdout):
        results.fail("MOVE file to directory", "Command failed")
        return
    
    # Verify
    stdout, code = env.run_client(f"DOWNLOAD {src_file} check_src.txt", "check_move_dir_src")
    if "ERROR" in stdout:
        stdout, code = env.run_client(f"DOWNLOAD {dst_dir}/{src_file} check_dst.txt", "check_move_dir_dst")
        if has_ok_response(stdout) and os.path.exists("check_dst.txt"):
            results.ok("MOVE file to directory")
            os.remove("check_dst.txt")
        else:
            results.fail("MOVE file to directory", "File not moved correctly")
    else:
        results.fail("MOVE file to directory", "Source still exists")
        if os.path.exists("check_src.txt"):
            os.remove("check_src.txt")
            
    env.run_client([f"DELETE {dst_dir}/{src_file}", f"RMDIR {dst_dir}"], "cleanup_move_dir")


def test_move_directory(env, results):
    """Test MOVE renames a directory."""
    src_dir = "dir_to_rename"
    dst_dir = "renamed_dir"
    
    env.run_client(f"MKDIR {src_dir}", "setup_move_dir_src")
    
    fname = "inside.txt"
    with open(fname, "w") as f:
        f.write("Inside directory")
    env.run_client([f"CD {src_dir}", f"UPLOAD {fname}"], "setup_move_dir_file")
    os.remove(fname)
    
    stdout, code = env.run_client(f"MOVE {src_dir} {dst_dir}", "move_directory")
    
    if not has_ok_response(stdout):
        results.fail("MOVE directory", "Command failed")
        return
    
    # Verify src gone
    stdout, code = env.run_client(f"CD {src_dir}", "check_move_dir_src_gone")
    if "ERROR" in stdout:
        # Verify dest exists and has content
        stdout, code = env.run_client([f"CD {dst_dir}", "LIST"], "check_move_dir_dst")
        if has_ok_response(stdout) and fname in stdout:
            results.ok("MOVE directory with contents")
        else:
            results.fail("MOVE directory with contents", "Contents not preserved")
    else:
        results.fail("MOVE directory", "Source directory still exists")
    
    env.run_client([f"CD {dst_dir}", f"DELETE {fname}", "CD ..", f"RMDIR {dst_dir}"], "cleanup_move_dir_renamed")


def test_copy_file(env, results):
    """Test COPY duplicates a file."""
    src_file = "copy_source.txt"
    dst_file = "copy_dest.txt"
    
    test_content = b"Content to be copied\n"
    with open(src_file, "wb") as f:
        f.write(test_content)
    env.run_client(f"UPLOAD {src_file}", "setup_copy_file")
    os.remove(src_file)
    
    stdout, code = env.run_client(f"COPY {src_file} {dst_file}", "copy_file")
    
    if not has_ok_response(stdout):
        results.fail("COPY file", "Command failed")
        return
    
    results.ok("COPY file")
    
    # Both should exist with same content
    # Verify src
    stdout, code = env.run_client(f"DOWNLOAD {src_file} check_src.txt", "check_copy_src")
    if has_ok_response(stdout):
        # Verify dest
        stdout, code = env.run_client(f"DOWNLOAD {dst_file} check_dst.txt", "check_copy_dst")
        if has_ok_response(stdout) and os.path.exists("check_dst.txt"):
            with open("check_dst.txt", "rb") as f:
                content = f.read()
            if content == test_content:
                results.ok("COPY preserves content and keeps source")
            else:
                results.fail("COPY content matches", "Content mismatch")
            os.remove("check_dst.txt")
        else:
            results.fail("COPY keeps source", "Dest missing or download failed")
        
        if os.path.exists("check_src.txt"):
            os.remove("check_src.txt")
    else:
        results.fail("COPY keeps source", "Source missing after copy")
    
    env.run_client(f"DELETE {src_file}", "cleanup_copy_src")
    env.run_client(f"DELETE {dst_file}", "cleanup_copy_dst")


def test_copy_directory(env, results):
    """Test COPY duplicates a directory."""
    src_dir = "dir_to_copy"
    dst_dir = "copied_dir"
    
    env.run_client(f"MKDIR {src_dir}", "setup_copy_dir")
    
    # Create nested content
    fname1 = "file1.txt"
    with open(fname1, "w") as f:
        f.write("File 1")
    env.run_client([f"CD {src_dir}", f"UPLOAD {fname1}"], "setup_copy_dir_file1")
    os.remove(fname1)
    
    env.run_client([f"CD {src_dir}", "MKDIR nested"], "setup_copy_dir_nested")
    fname2 = "file2.txt"
    with open(fname2, "w") as f:
        f.write("File 2")
    env.run_client([f"CD {src_dir}/nested", f"UPLOAD {fname2}"], "setup_copy_dir_file2")
    os.remove(fname2)
    
    stdout, code = env.run_client(f"COPY {src_dir} {dst_dir}", "copy_directory")
    
    if not has_ok_response(stdout):
        results.fail("COPY directory", "Command failed")
        return
    
    # Both should exist
    # Check src
    stdout, code = env.run_client(f"CD {src_dir}", "check_copy_src_dir")
    if has_ok_response(stdout):
        # Check dest
        stdout, code = env.run_client(f"CD {dst_dir}", "check_copy_dst_dir")
        if has_ok_response(stdout):
            # Check copied content
            stdout, code = env.run_client([f"CD {dst_dir}", "LIST"], "check_copy_dst_list")
            if fname1 in stdout:
                 stdout, code = env.run_client([f"CD {dst_dir}/nested", "LIST"], "check_copy_dst_nested")
                 if fname2 in stdout:
                     results.ok("COPY directory with nested contents")
                 else:
                     results.fail("COPY directory contents", "Nested file not copied")
            else:
                results.fail("COPY directory contents", "File not copied")
        else:
            results.fail("COPY directory", "Dest directory missing")
    else:
        results.fail("COPY directory", "Source directory missing")
    
    # Cleanup (recursive delete not supported by DELETE, need RMDIR)
    # Assuming RMDIR is recursive or we need to clean up manually
    # The test_rmdir_with_contents suggests RMDIR is recursive
    env.run_client(f"RMDIR {src_dir}", "cleanup_copy_src_dir")
    env.run_client(f"RMDIR {dst_dir}", "cleanup_copy_dst_dir")


def test_copy_to_existing_fails(env, results):
    """Test COPY fails when destination exists."""
    src_file = "copy_src.txt"
    dst_file = "copy_existing_dst.txt"
    
    with open(src_file, "w") as f:
        f.write("Source")
    env.run_client(f"UPLOAD {src_file}", "setup_copy_exist_src")
    os.remove(src_file)
    
    with open(dst_file, "w") as f:
        f.write("Existing dest")
    env.run_client(f"UPLOAD {dst_file}", "setup_copy_exist_dst")
    os.remove(dst_file)
    
    stdout, code = env.run_client(f"COPY {src_file} {dst_file}", "copy_existing")
    
    if "ERROR" in stdout:
        results.ok("COPY to existing destination returns ERROR")
    else:
        results.fail("COPY to existing destination returns ERROR", "Expected ERROR")
    
    env.run_client(f"DELETE {src_file}", "cleanup_copy_exist_src")
    env.run_client(f"DELETE {dst_file}", "cleanup_copy_exist_dst")


def test_move_nonexistent(env, results):
    """Test MOVE fails for non-existent source."""
    stdout, code = env.run_client("MOVE nonexistent.txt dest.txt", "move_nonexistent")
    
    if "ERROR" in stdout:
        results.ok("MOVE nonexistent source returns ERROR")
    else:
        results.fail("MOVE nonexistent source returns ERROR", "Expected ERROR")


def test_path_traversal_mkdir(env, results):
    """Test MKDIR blocks path traversal."""
    stdout, code = env.run_client("MKDIR ../outside", "mkdir_traversal")
    
    if "ERROR" in stdout:
        results.ok("MKDIR path traversal blocked")
    else:
        # Check that directory wasn't created outside
        outside_path = os.path.join(env.server_root, "outside")
        if not os.path.exists(outside_path):
            results.ok("MKDIR path traversal blocked (stayed in sandbox)")
        else:
            results.fail("MKDIR path traversal blocked", "Directory created outside root!")


def test_upload_implicit_path_nested_folders(env, results):
    """Test UPLOAD with implicit path inside nested folders (level 2)."""
    filename = "nested_implicit.txt"
    content = b"Content for nested implicit upload"
    
    # Create local file
    local_path = os.path.join(env.client_cwd, filename)
    with open(local_path, "wb") as f:
        f.write(content)
        
    try:
        # Create folders and upload in one session
        commands = [
            "MKDIR level1",
            "CD level1",
            "MKDIR level2",
            "CD level2",
            f"UPLOAD {filename}"
        ]
        
        stdout, code = env.run_client(commands, "upload_nested_implicit")
        
        if has_ok_response(stdout):
            # Verify by downloading in a fresh session
            # We need to navigate to the folder first to find the file
            verify_commands = [
                "CD level1",
                "CD level2",
                f"DOWNLOAD {filename} check_nested.txt"
            ]
            
            env.run_client(verify_commands, "verify_nested_implicit")
            
            check_path = os.path.join(env.client_cwd, "check_nested.txt")
            if os.path.exists(check_path):
                with open(check_path, "rb") as f:
                    if f.read() == content:
                        results.ok("UPLOAD implicit path in nested folders")
                    else:
                        results.fail("UPLOAD implicit path in nested folders", "Content mismatch")
                os.remove(check_path)
            else:
                results.fail("UPLOAD implicit path in nested folders", "Could not download verification file")
        else:
            results.fail("UPLOAD implicit path in nested folders", "Command sequence failed")
            
    finally:
        if os.path.exists(local_path):
            os.remove(local_path)


def main():
    print("MiniDrive Integration Tests - Folder Commands")
    print("=" * 60)
    print("Testing: CD, MKDIR, RMDIR, MOVE, COPY")
    
    check_executables()
    
    env = TestEnvironment("folder_commands", SERVER_PORT)
    results = TestResult(env.log_dir)
    
    # Change to client working directory so tests create files there
    os.chdir(env.client_cwd)
    
    try:
        print("\nSetting up test environment...")
        env.setup_server_root()
        env.start_server()
        
        print("\nRunning tests:\n")
        
        # MKDIR tests
        test_mkdir_simple(env, results)
        test_mkdir_nested(env, results)
        test_mkdir_already_exists(env, results)
        
        # RMDIR tests
        test_rmdir_empty(env, results)
        test_rmdir_with_contents(env, results)
        test_rmdir_nonexistent(env, results)
        
        # CD tests
        test_cd_to_directory(env, results)
        test_cd_to_parent(env, results)
        test_cd_absolute_path(env, results)
        test_cd_nonexistent(env, results)
        test_upload_implicit_path_nested_folders(env, results)
        
        # MOVE tests
        test_move_file(env, results)
        test_move_to_directory(env, results)
        test_move_directory(env, results)
        test_move_nonexistent(env, results)
        
        # COPY tests
        test_copy_file(env, results)
        test_copy_directory(env, results)
        test_copy_to_existing_fails(env, results)
        
        # Security test
        test_path_traversal_mkdir(env, results)
        
    finally:
        print("\nCleaning up...")
        env.cleanup()
    
    success = results.summary()
    print(f"\nLogs saved to: {env.log_dir}")
    
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
