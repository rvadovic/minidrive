#!/usr/bin/env python3
"""
Integration tests for MiniDrive batch commands and two-way SYNC behaviour.

test_sync_command.py covers the one-way local->remote contract from the requirements. This suite
covers everything the three-way merge adds on top of it, plus the batch/directory commands:
- SYNC pulls remote-only changes down (two-way)
- SYNC keeps both versions when both sides changed (conflict copy)
- SYNC turns a local rename into a server-side MOVE instead of delete + re-upload
- SYNC turns a local duplicate into a server-side COPY instead of a second upload
- UPLOAD_DIR / DOWNLOAD_DIR whole-directory transfers
- Multi-argument DELETE / MOVE / COPY
- One failing item does not abort the rest of a batch

Note on assertions: has_ok_response() matches "ok" anywhere in stdout, including the public-mode
banner every client run prints, so it can never prove a *single* command succeeded. Every check
below is therefore anchored on real evidence - file contents, file existence, or the summary line
the batch engine prints.

Usage:
    python3 tests/integration/test_batch_and_twoway_sync.py
"""

import os
import re
import sys
import shutil

from test_utils import (
    TestEnvironment, TestResult, check_executables, calculate_hash
)

SERVER_PORT = 9033


def make_local_dir(name):
    path = os.path.abspath(name)
    if os.path.exists(path):
        shutil.rmtree(path)
    os.makedirs(path)
    return path


def write(path, content):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(content if isinstance(content, bytes) else content.encode())


def read(path):
    with open(path, "rb") as f:
        return f.read()


def summary_counts(stdout):
    """Parse the batch summary line into {'uploaded': n, 'moved': n, ...}."""
    match = re.search(r"complete\.(.*)", stdout)
    if not match:
        return {}
    counts = {}
    for key, value in re.findall(r"([A-Za-z ]+):\s*(\d+)", match.group(1)):
        counts[key.strip().lower().split(" ")[0]] = int(value)
    return counts


def server_path(env, *parts):
    """Path of a file inside the public user root on the server."""
    return os.path.join(env.server_root, "public", "files", *parts)


def test_sync_pulls_remote_changes(env, results):
    """A file that only changed on the server comes back down to the local folder."""
    local_dir = make_local_dir("tw_pull")
    remote_dir = "tw_pull_remote"
    env.run_client(f"MKDIR {remote_dir}", "pull_mkdir")

    write(os.path.join(local_dir, "shared.txt"), "original\n")

    try:
        env.run_client(f"SYNC {local_dir} {remote_dir}", "pull_initial")

        # Change the file on the server side only, behind the client's back
        write(server_path(env, remote_dir, "shared.txt"), "changed on server\n")
        write(server_path(env, remote_dir, "server_only.txt"), "only on server\n")

        stdout, _ = env.run_client(f"SYNC {local_dir} {remote_dir}", "pull_second")
        counts = summary_counts(stdout)

        if read(os.path.join(local_dir, "shared.txt")) == b"changed on server\n":
            results.ok("SYNC pulls a server-side edit down to the local folder")
        else:
            results.fail("SYNC pulls server edit", "Local file was not updated")

        if os.path.exists(os.path.join(local_dir, "server_only.txt")):
            results.ok("SYNC pulls a server-only new file down")
        else:
            results.fail("SYNC pulls new server file", "File not created locally")

        if counts.get("downloaded", 0) == 2:
            results.ok("SYNC summary reports both downloads")
        else:
            results.fail("SYNC download count", f"Expected 2 downloads, got {counts}")
    finally:
        shutil.rmtree(local_dir, ignore_errors=True)


def test_sync_conflict_keeps_both_versions(env, results):
    """Both sides edited the same file: local wins the path, the server version is kept beside it."""
    local_dir = make_local_dir("tw_conflict")
    remote_dir = "tw_conflict_remote"
    env.run_client(f"MKDIR {remote_dir}", "conflict_mkdir")

    write(os.path.join(local_dir, "both.txt"), "base\n")

    try:
        env.run_client(f"SYNC {local_dir} {remote_dir}", "conflict_initial")

        write(os.path.join(local_dir, "both.txt"), "local edit\n")
        write(server_path(env, remote_dir, "both.txt"), "server edit\n")

        stdout, _ = env.run_client(f"SYNC {local_dir} {remote_dir}", "conflict_second")

        if "CONFLICT" in stdout:
            results.ok("SYNC reports the conflict")
        else:
            results.fail("SYNC conflict report", "No conflict line in output")

        if read(server_path(env, remote_dir, "both.txt")) == b"local edit\n":
            results.ok("SYNC conflict keeps the local version on the canonical remote path")
        else:
            results.fail("SYNC conflict remote side", "Server does not hold the local version")

        copies = [n for n in os.listdir(local_dir) if "conflict copy" in n]
        if len(copies) == 1 and read(os.path.join(local_dir, copies[0])) == b"server edit\n":
            results.ok("SYNC conflict saves the server version as a local conflict copy")
        else:
            results.fail("SYNC conflict copy", f"Expected one conflict copy holding the server version, found {copies}")
    finally:
        shutil.rmtree(local_dir, ignore_errors=True)


def test_sync_detects_move(env, results):
    """A local rename becomes a server-side MOVE, not a delete plus a fresh upload."""
    local_dir = make_local_dir("tw_move")
    remote_dir = "tw_move_remote"
    env.run_client(f"MKDIR {remote_dir}", "move_mkdir")

    payload = os.urandom(4096)
    write(os.path.join(local_dir, "before.bin"), payload)

    try:
        env.run_client(f"SYNC {local_dir} {remote_dir}", "move_initial")

        os.makedirs(os.path.join(local_dir, "sub"))
        os.rename(os.path.join(local_dir, "before.bin"), os.path.join(local_dir, "sub", "after.bin"))

        stdout, _ = env.run_client(f"SYNC {local_dir} {remote_dir}", "move_second")
        counts = summary_counts(stdout)

        moved_ok = (not os.path.exists(server_path(env, remote_dir, "before.bin"))
                    and os.path.exists(server_path(env, remote_dir, "sub", "after.bin"))
                    and read(server_path(env, remote_dir, "sub", "after.bin")) == payload)

        if moved_ok:
            results.ok("SYNC applies a local rename on the server")
        else:
            results.fail("SYNC rename", "Server tree does not reflect the rename")

        if counts.get("moved", 0) == 1 and counts.get("uploaded", 0) == 0:
            results.ok("SYNC renames without re-uploading the content")
        else:
            results.fail("SYNC move detection", f"Expected 1 move and 0 uploads, got {counts}")
    finally:
        shutil.rmtree(local_dir, ignore_errors=True)


def test_sync_detects_copy(env, results):
    """A local duplicate of an already-synced file becomes a server-side COPY."""
    local_dir = make_local_dir("tw_copy")
    remote_dir = "tw_copy_remote"
    env.run_client(f"MKDIR {remote_dir}", "copy_mkdir")

    payload = os.urandom(4096)
    write(os.path.join(local_dir, "original.bin"), payload)

    try:
        env.run_client(f"SYNC {local_dir} {remote_dir}", "copy_initial")

        shutil.copyfile(os.path.join(local_dir, "original.bin"),
                        os.path.join(local_dir, "duplicate.bin"))

        stdout, _ = env.run_client(f"SYNC {local_dir} {remote_dir}", "copy_second")
        counts = summary_counts(stdout)

        if os.path.exists(server_path(env, remote_dir, "duplicate.bin")) and \
           read(server_path(env, remote_dir, "duplicate.bin")) == payload:
            results.ok("SYNC creates the duplicate on the server")
        else:
            results.fail("SYNC duplicate", "Duplicate missing or wrong content on the server")

        if counts.get("copied", 0) == 1 and counts.get("uploaded", 0) == 0:
            results.ok("SYNC duplicates server-side instead of uploading identical bytes")
        else:
            results.fail("SYNC copy optimisation", f"Expected 1 copy and 0 uploads, got {counts}")
    finally:
        shutil.rmtree(local_dir, ignore_errors=True)


def test_upload_dir(env, results):
    """UPLOAD_DIR pushes a whole local tree in one command."""
    local_dir = make_local_dir("batch_updir")
    remote_dir = "batch_updir_remote"

    files = {
        "top.txt": b"top\n",
        "a/one.txt": b"one\n",
        "a/b/two.bin": os.urandom(2048),
    }
    for name, content in files.items():
        write(os.path.join(local_dir, name), content)

    try:
        stdout, _ = env.run_client(f"UPLOAD_DIR {local_dir} {remote_dir}", "upload_dir", timeout=60)
        counts = summary_counts(stdout)

        all_there = all(
            os.path.exists(server_path(env, remote_dir, *name.split("/")))
            and read(server_path(env, remote_dir, *name.split("/"))) == content
            for name, content in files.items()
        )

        if all_there:
            results.ok("UPLOAD_DIR uploads the whole tree with correct content")
        else:
            results.fail("UPLOAD_DIR", "Some files missing or wrong on the server")

        if counts.get("uploaded", 0) == 3 and counts.get("failed", 0) == 0:
            results.ok("UPLOAD_DIR summary reports every file")
        else:
            results.fail("UPLOAD_DIR summary", f"Expected 3 uploads and 0 failures, got {counts}")
    finally:
        shutil.rmtree(local_dir, ignore_errors=True)


def test_download_dir(env, results):
    """DOWNLOAD_DIR pulls a whole remote tree in one command."""
    source_dir = make_local_dir("batch_downsrc")
    target_dir = os.path.abspath("batch_downdst")
    shutil.rmtree(target_dir, ignore_errors=True)
    remote_dir = "batch_downdir_remote"

    files = {
        "root.txt": b"root\n",
        "nested/deep.bin": os.urandom(3000),
    }
    for name, content in files.items():
        write(os.path.join(source_dir, name), content)

    try:
        env.run_client(f"UPLOAD_DIR {source_dir} {remote_dir}", "download_dir_seed", timeout=60)

        stdout, _ = env.run_client(f"DOWNLOAD_DIR {remote_dir} {target_dir}", "download_dir", timeout=60)
        counts = summary_counts(stdout)

        all_there = all(
            os.path.exists(os.path.join(target_dir, *name.split("/")))
            and read(os.path.join(target_dir, *name.split("/"))) == content
            for name, content in files.items()
        )

        if all_there:
            results.ok("DOWNLOAD_DIR recreates the remote tree locally")
        else:
            results.fail("DOWNLOAD_DIR", "Some files missing or wrong locally")

        if counts.get("downloaded", 0) == 2 and counts.get("failed", 0) == 0:
            results.ok("DOWNLOAD_DIR summary reports every file")
        else:
            results.fail("DOWNLOAD_DIR summary", f"Expected 2 downloads and 0 failures, got {counts}")
    finally:
        shutil.rmtree(source_dir, ignore_errors=True)
        shutil.rmtree(target_dir, ignore_errors=True)


def test_batch_delete_tolerates_failures(env, results):
    """Multi-argument DELETE removes every valid target even when one argument is bogus."""
    local_dir = make_local_dir("batch_del")
    remote_dir = "batch_del_remote"

    for name in ["d1.txt", "d2.txt", "keep.txt"]:
        write(os.path.join(local_dir, name), f"{name}\n")

    try:
        env.run_client(f"UPLOAD_DIR {local_dir} {remote_dir}", "batch_del_seed", timeout=60)

        stdout, _ = env.run_client(
            f"DELETE {remote_dir}/d1.txt {remote_dir}/d2.txt {remote_dir}/missing.txt",
            "batch_delete")
        counts = summary_counts(stdout)

        gone = (not os.path.exists(server_path(env, remote_dir, "d1.txt"))
                and not os.path.exists(server_path(env, remote_dir, "d2.txt")))
        kept = os.path.exists(server_path(env, remote_dir, "keep.txt"))

        if gone and kept:
            results.ok("Batch DELETE removes every named file and nothing else")
        else:
            results.fail("Batch DELETE", "Wrong set of files removed")

        if counts.get("deleted", 0) == 2 and counts.get("failed", 0) == 1:
            results.ok("Batch DELETE finishes the queue despite one failing item")
        else:
            results.fail("Batch DELETE failure tolerance", f"Expected 2 deleted and 1 failed, got {counts}")
    finally:
        shutil.rmtree(local_dir, ignore_errors=True)


def test_batch_move_and_copy_into_directory(env, results):
    """Several sources plus a directory destination expand into one request per item."""
    local_dir = make_local_dir("batch_mv")
    remote_dir = "batch_mv_remote"

    for name in ["m1.txt", "m2.txt"]:
        write(os.path.join(local_dir, name), f"{name}\n")

    try:
        env.run_client(f"UPLOAD_DIR {local_dir} {remote_dir}", "batch_mv_seed", timeout=60)
        env.run_client(f"MKDIR {remote_dir}/copies", "batch_mv_mkdir_copies")
        env.run_client(f"MKDIR {remote_dir}/moved", "batch_mv_mkdir_moved")

        stdout, _ = env.run_client(
            f"COPY {remote_dir}/m1.txt {remote_dir}/m2.txt {remote_dir}/copies/", "batch_copy")
        counts = summary_counts(stdout)

        copied_ok = (os.path.exists(server_path(env, remote_dir, "copies", "m1.txt"))
                     and os.path.exists(server_path(env, remote_dir, "copies", "m2.txt"))
                     and os.path.exists(server_path(env, remote_dir, "m1.txt")))

        if copied_ok and counts.get("copied", 0) == 2:
            results.ok("Batch COPY copies every source into the destination directory")
        else:
            results.fail("Batch COPY", f"Unexpected result, counts={counts}")

        stdout, _ = env.run_client(
            f"MOVE {remote_dir}/m1.txt {remote_dir}/m2.txt {remote_dir}/moved/", "batch_move")
        counts = summary_counts(stdout)

        moved_ok = (os.path.exists(server_path(env, remote_dir, "moved", "m1.txt"))
                    and os.path.exists(server_path(env, remote_dir, "moved", "m2.txt"))
                    and not os.path.exists(server_path(env, remote_dir, "m1.txt")))

        if moved_ok and counts.get("moved", 0) == 2:
            results.ok("Batch MOVE moves every source into the destination directory")
        else:
            results.fail("Batch MOVE", f"Unexpected result, counts={counts}")
    finally:
        shutil.rmtree(local_dir, ignore_errors=True)


def test_single_argument_forms_unchanged(env, results):
    """One source and a plain destination keep the classic single-item rename/copy behaviour."""
    local_dir = make_local_dir("batch_single")
    remote_dir = "batch_single_remote"
    write(os.path.join(local_dir, "s.txt"), "single\n")

    try:
        env.run_client(f"UPLOAD_DIR {local_dir} {remote_dir}", "single_seed", timeout=60)

        env.run_client(f"MOVE {remote_dir}/s.txt {remote_dir}/renamed.txt", "single_move")
        env.run_client(f"COPY {remote_dir}/renamed.txt {remote_dir}/copied.txt", "single_copy")

        renamed = os.path.exists(server_path(env, remote_dir, "renamed.txt"))
        copied = os.path.exists(server_path(env, remote_dir, "copied.txt"))
        original_gone = not os.path.exists(server_path(env, remote_dir, "s.txt"))

        if renamed and copied and original_gone:
            results.ok("Single-argument MOVE and COPY still rename in place")
        else:
            results.fail("Single-argument MOVE/COPY", "Classic rename behaviour changed")
    finally:
        shutil.rmtree(local_dir, ignore_errors=True)


def test_sync_large_file_two_way(env, results):
    """A larger file survives a full round trip through the batch engine."""
    local_dir = make_local_dir("tw_large")
    target_dir = os.path.abspath("tw_large_back")
    shutil.rmtree(target_dir, ignore_errors=True)
    remote_dir = "tw_large_remote"
    env.run_client(f"MKDIR {remote_dir}", "large_mkdir")

    path = os.path.join(local_dir, "big.bin")
    write(path, os.urandom(3 * 1024 * 1024))
    original = calculate_hash(path)

    try:
        env.run_client(f"SYNC {local_dir} {remote_dir}", "large_sync", timeout=120)
        env.run_client(f"DOWNLOAD_DIR {remote_dir} {target_dir}", "large_back", timeout=120)

        back = os.path.join(target_dir, "big.bin")
        if os.path.exists(back) and calculate_hash(back) == original:
            results.ok("SYNC then DOWNLOAD_DIR round-trips a large file intact")
        else:
            results.fail("Large file round trip", "Hash mismatch or file missing")
    finally:
        shutil.rmtree(local_dir, ignore_errors=True)
        shutil.rmtree(target_dir, ignore_errors=True)


def main():
    print("MiniDrive Integration Tests - Batch Commands and Two-Way Sync")
    print("=" * 60)

    check_executables()

    env = TestEnvironment("batch_twoway", SERVER_PORT)
    results = TestResult(env.log_dir)

    os.chdir(env.client_cwd)

    try:
        print("\nSetting up test environment...")
        env.setup_server_root()
        env.start_server()

        print("\nRunning tests:\n")

        test_sync_pulls_remote_changes(env, results)
        test_sync_conflict_keeps_both_versions(env, results)
        test_sync_detects_move(env, results)
        test_sync_detects_copy(env, results)

        test_upload_dir(env, results)
        test_download_dir(env, results)
        test_batch_delete_tolerates_failures(env, results)
        test_batch_move_and_copy_into_directory(env, results)
        test_single_argument_forms_unchanged(env, results)

        test_sync_large_file_two_way(env, results)

    finally:
        print("\nCleaning up...")
        env.cleanup()

    success = results.summary()
    print(f"\nLogs saved to: {env.log_dir}")

    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
