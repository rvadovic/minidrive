#!/usr/bin/env python3
"""Integration tests for the end-to-end encrypted vault.

These deliberately do more than "an encrypted upload comes back intact" - that would pass just as
well with the encryption switched off. The assertions that matter look at what the server actually
holds on disk: that the stored bytes are not the plaintext, that the plaintext appears nowhere
under the server root, and that a tampered stored file is refused rather than handed back.

Also covered: the bookkeeping that keeps a vaulted account usable rather than merely secret - SYNC
staying idempotent (which only works because the server reports the plaintext hash it was given),
MOVE/COPY/DELETE/RMDIR keeping per-file keys in step with the files, resumability, hybrid device
enrollment, and plain accounts being completely unaffected.

Assertions look at filesystem state rather than has_ok_response(), which matches the bare substring
"ok" anywhere in a transcript - including the login banner.

Usage:
    python3 tests/integration/test_vault.py
"""

import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time

from test_utils import (
    TestEnvironment,
    TestResult,
    check_executables,
)

SERVER_PORT = 9040


def error_codes(stdout: str):
    """Every protocol error code in a transcript, as the client actually prints them."""
    return [int(code) for code in re.findall(r"ERROR:\s*<(\d+)>", stdout or "")]


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(65536), b""):
            h.update(block)
    return h.hexdigest()


class VaultTestEnvironment(TestEnvironment):
    """Adds private-mode helpers, and lets a test keep one client working directory across runs
    (which is what makes an enrolled device's stored keys survive to the next login)."""

    def _run_client(self, connect_arg, input_lines, test_name, cwd=None, timeout=120):
        self.test_counter += 1
        log_prefix = f"{self.test_counter:02d}_{test_name}"
        stdout_log = os.path.join(self.log_dir, f"{log_prefix}_stdout.log")
        work_dir = cwd or os.path.join(self.log_dir, f"{log_prefix}_workdir")
        os.makedirs(work_dir, exist_ok=True)

        process = self.start_client_process(
            connect_arg, os.path.join(self.log_dir, f"{log_prefix}_client.log"), cwd=work_dir
        )
        try:
            stdout, _ = process.communicate(input="\n".join(input_lines) + "\n", timeout=timeout)
        except subprocess.TimeoutExpired:
            process.kill()
            stdout, _ = process.communicate()
            with open(stdout_log, "w") as f:
                f.write(f"# TIMEOUT after {timeout}s\n{stdout}")
            return "TIMEOUT", work_dir

        with open(stdout_log, "w") as f:
            f.write(f"# Connect: {connect_arg}\n# Input: {input_lines}\n{'='*50}\n\n")
            f.write(stdout)
        return stdout, work_dir

    def register(self, username, password, test_name):
        return self._run_client(f"{username}@127.0.0.1:{self.port}", ["y", password, "EXIT"], test_name)

    def run_user(self, username, password, commands, test_name, cwd=None, timeout=120):
        if isinstance(commands, str):
            commands = [commands]
        return self._run_client(
            f"{username}@127.0.0.1:{self.port}", [password] + commands + ["EXIT"], test_name, cwd, timeout
        )

    def run_public(self, commands, test_name, timeout=120):
        if isinstance(commands, str):
            commands = [commands]
        return self._run_client(f"127.0.0.1:{self.port}", commands + ["EXIT"], test_name, timeout=timeout)

    # Server-side paths, so tests can inspect what is really stored
    def user_dir(self, username):
        return os.path.join(self.server_root, "private", username)

    def files_dir(self, username):
        return os.path.join(self.user_dir(username), "files")

    def vault_json(self, username):
        return os.path.join(self.user_dir(username), ".vault", "vault.json")

    def dek_manifest(self, username):
        path = os.path.join(self.user_dir(username), ".vault", "dek_manifest.json")
        if not os.path.exists(path):
            return {}
        with open(path) as f:
            return json.load(f).get("entries", {})

    def with_vault(self, username, password, test_name):
        """Register a user and turn on end-to-end encryption for them."""
        self.register(username, password, f"register_{test_name}")
        return self.run_user(username, password, "VAULT_INIT", f"vault_init_{test_name}")


def make_local(env, name, content):
    """Write a local file for a client to upload, in a scratch directory of its own."""
    scratch = os.path.join(env.log_dir, "scratch")
    os.makedirs(scratch, exist_ok=True)
    path = os.path.join(scratch, name)
    mode = "wb" if isinstance(content, bytes) else "w"
    with open(path, mode) as f:
        f.write(content)
    return path


# --------------------------------------------------------------------------- tests


def test_vault_init_creates_key_material(env, results):
    env.register("valerie", "pw_valerie", "register_valerie")
    stdout, _ = env.run_user("valerie", "pw_valerie", "VAULT_INIT", "vault_init")

    if not os.path.exists(env.vault_json("valerie")):
        results.fail("VAULT_INIT creates a vault", f"no vault.json was written: {stdout!r}")
        return
    results.ok("VAULT_INIT creates a vault")

    with open(env.vault_json("valerie")) as f:
        vault = json.load(f)

    missing = [k for k in ("vault_salt", "argon2_opslimit", "argon2_memlimit", "wrapped_vk_password")
               if k not in vault]
    if missing:
        results.fail("Vault records salt and parameters", f"missing fields: {missing}")
        return
    results.ok("Vault records salt and parameters")

    # The password must not be recoverable from anything the server holds
    blob = json.dumps(vault)
    if "pw_valerie" in blob:
        results.fail("Vault stores no password", "the password appears in vault.json")
        return
    results.ok("Vault stores no password")


def test_second_vault_init_is_refused(env, results):
    stdout, _ = env.run_user("valerie", "pw_valerie", "VAULT_INIT", "vault_init_twice")

    # Re-initializing would mint a new vault key and orphan everything sealed under the old one
    if 409 not in error_codes(stdout):
        results.fail("Second VAULT_INIT is refused", f"expected 409, got {error_codes(stdout)}")
        return
    results.ok("Second VAULT_INIT is refused")


def test_public_mode_has_no_vault(env, results):
    stdout, _ = env.run_public("VAULT_INIT", "vault_init_public")

    codes = error_codes(stdout)
    if not codes:
        results.fail("Public mode cannot create a vault", f"expected an error, got {stdout!r}")
        return
    if os.path.exists(os.path.join(env.server_root, "public", ".vault", "vault.json")):
        results.fail("Public mode cannot create a vault", "a vault was created for public mode")
        return
    results.ok("Public mode cannot create a vault")


def test_server_stores_ciphertext_only(env, results):
    env.with_vault("carol", "pw_carol", "carol")

    secret = "the treasure is buried under the third oak"
    local = make_local(env, "secret.txt", secret)
    env.run_user("carol", "pw_carol", f"UPLOAD {local} secret.txt", "upload_secret")

    stored = os.path.join(env.files_dir("carol"), "secret.txt")
    if not os.path.exists(stored):
        results.fail("Encrypted upload is stored", "no file on the server")
        return
    results.ok("Encrypted upload is stored")

    with open(stored, "rb") as f:
        stored_bytes = f.read()

    if secret.encode() in stored_bytes:
        results.fail("Stored file is not the plaintext", "the plaintext is sitting in the stored file")
        return
    results.ok("Stored file is not the plaintext")

    # Ciphertext is the plaintext plus one Poly1305 tag per chunk; one chunk here
    if len(stored_bytes) != len(secret) + 16:
        results.fail("Stored file is sealed per chunk",
                     f"expected {len(secret) + 16} bytes, got {len(stored_bytes)}")
        return
    results.ok("Stored file is sealed per chunk")

    # Nothing anywhere under the server root should contain it, manifest and metadata included
    leaked = []
    for dirpath, _, filenames in os.walk(env.server_root):
        for name in filenames:
            path = os.path.join(dirpath, name)
            try:
                with open(path, "rb") as f:
                    if secret.encode() in f.read():
                        leaked.append(path)
            except OSError:
                pass
    if leaked:
        results.fail("Plaintext appears nowhere on the server", f"found in {leaked}")
        return
    results.ok("Plaintext appears nowhere on the server")


def test_round_trip_is_byte_identical(env, results):
    payload = os.urandom(700 * 1024)  # several chunks, so chunk indexing and the last-chunk flag matter
    local = make_local(env, "payload.bin", payload)
    env.run_user("carol", "pw_carol", f"UPLOAD {local} payload.bin", "upload_payload")

    stdout, work = env.run_user("carol", "pw_carol",
                                f"DOWNLOAD payload.bin {os.path.join('', 'payload_back.bin')}",
                                "download_payload")
    back = os.path.join(work, "payload_back.bin")

    if not os.path.exists(back):
        results.fail("Encrypted round trip returns the file", f"nothing downloaded: {stdout!r}")
        return
    if md5(back) != md5(local):
        results.fail("Encrypted round trip returns the file", "the downloaded file differs from the source")
        return
    results.ok("Encrypted round trip returns the file")

    stored = os.path.join(env.files_dir("carol"), "payload.bin")
    if os.path.getsize(stored) <= len(payload):
        results.fail("Multi-chunk file is sealed chunk by chunk",
                     f"stored size {os.path.getsize(stored)} is not larger than the plaintext")
        return
    results.ok("Multi-chunk file is sealed chunk by chunk")


def test_manifest_records_keys_not_content(env, results):
    entries = env.dek_manifest("carol")

    if "secret.txt" not in entries or "payload.bin" not in entries:
        results.fail("Manifest has an entry per stored file", f"entries: {sorted(entries)}")
        return
    results.ok("Manifest has an entry per stored file")

    entry = entries["secret.txt"]
    if not entry.get("wrapped_dek", {}).get("ciphertext") or not entry.get("plaintext_hash"):
        results.fail("Manifest entry carries a sealed key and a plaintext hash", f"entry: {entry}")
        return
    results.ok("Manifest entry carries a sealed key and a plaintext hash")


def test_tampered_ciphertext_is_refused(env, results):
    local = make_local(env, "tamper.txt", "authenticity matters")
    env.run_user("carol", "pw_carol", f"UPLOAD {local} tamper.txt", "upload_tamper")

    stored = os.path.join(env.files_dir("carol"), "tamper.txt")
    with open(stored, "r+b") as f:
        data = bytearray(f.read())
        data[2] ^= 0x01  # one bit, in the ciphertext the server holds
        f.seek(0)
        f.write(data)

    stdout, work = env.run_user("carol", "pw_carol", "DOWNLOAD tamper.txt tampered_out.txt",
                                "download_tampered")

    if not error_codes(stdout):
        results.fail("A tampered file is refused", f"the download reported no error: {stdout!r}")
        return
    results.ok("A tampered file is refused")

    # A partly-decrypted file must not be left looking like a successful download
    if os.path.exists(os.path.join(work, "tampered_out.txt")):
        results.fail("A tampered file leaves nothing behind", "a destination file was written anyway")
        return
    results.ok("A tampered file leaves nothing behind")


def test_other_user_cannot_read_the_file(env, results):
    env.with_vault("mallory", "pw_mallory", "mallory")

    # Traversal is already blocked, and the vault adds that even reaching the bytes yields nothing
    stdout, work = env.run_user("mallory", "pw_mallory",
                                "DOWNLOAD ../../carol/files/secret.txt stolen.txt", "cross_user")

    if os.path.exists(os.path.join(work, "stolen.txt")):
        results.fail("Another user cannot read the file", "the file was retrieved across accounts")
        return
    if not error_codes(stdout):
        results.fail("Another user cannot read the file", f"expected an error, got {stdout!r}")
        return
    results.ok("Another user cannot read the file")

    # And their vault keys are separate: mallory's manifest knows nothing about carol's files
    if env.dek_manifest("mallory"):
        results.fail("Vaults are per account", "mallory's manifest is not empty")
        return
    results.ok("Vaults are per account")


def test_sync_is_idempotent_for_a_vaulted_account(env, results):
    """The second SYNC must skip everything.

    This only works because the server reports the plaintext hash the client recorded at upload
    time. Hashing the stored ciphertext would give a different answer every time - independent
    per-file keys mean identical content produces different bytes - and SYNC would re-upload the
    whole tree on every run, forever.
    """
    env.with_vault("sam", "pw_sam", "sam")

    tree = os.path.join(env.log_dir, "sync_tree")
    os.makedirs(os.path.join(tree, "sub"), exist_ok=True)
    with open(os.path.join(tree, "a.txt"), "w") as f:
        f.write("alpha")
    with open(os.path.join(tree, "sub", "b.txt"), "w") as f:
        f.write("beta")

    stdout, _ = env.run_user("sam", "pw_sam",
                             ["MKDIR remote", f"SYNC {tree} remote", f"SYNC {tree} remote"],
                             "sync_twice")

    summaries = re.findall(r"Sync complete\..*", stdout or "")
    if len(summaries) != 2:
        results.fail("SYNC runs on a vaulted account", f"expected two summaries, got: {summaries}")
        return

    if "Uploaded: 2" not in summaries[0]:
        results.fail("First SYNC uploads the tree", summaries[0])
        return
    results.ok("First SYNC uploads the tree")

    if "Uploaded: 0" not in summaries[1] or "Skipped (unchanged): 2" not in summaries[1]:
        results.fail("Second SYNC skips everything", summaries[1])
        return
    results.ok("Second SYNC skips everything")

    stored = os.path.join(env.files_dir("sam"), "remote", "a.txt")
    with open(stored, "rb") as f:
        if b"alpha" in f.read():
            results.fail("Synced files are encrypted too", "the plaintext is in the stored file")
            return
    results.ok("Synced files are encrypted too")


def test_download_dir_reconstructs_the_tree(env, results):
    out = os.path.join(env.log_dir, "pulled_tree")
    env.run_user("sam", "pw_sam", f"DOWNLOAD_DIR remote {out}", "download_dir")

    a = os.path.join(out, "a.txt")
    b = os.path.join(out, "sub", "b.txt")
    if not os.path.exists(a) or not os.path.exists(b):
        results.fail("DOWNLOAD_DIR decrypts a whole tree", f"missing files under {out}")
        return
    if open(a).read() != "alpha" or open(b).read() != "beta":
        results.fail("DOWNLOAD_DIR decrypts a whole tree", "decrypted contents differ")
        return
    results.ok("DOWNLOAD_DIR decrypts a whole tree")


def test_move_and_copy_carry_the_key(env, results):
    stdout, work = env.run_user("carol", "pw_carol", [
        "MOVE secret.txt renamed.txt",
        "COPY renamed.txt duplicate.txt",
        "DOWNLOAD renamed.txt moved_out.txt",
        "DOWNLOAD duplicate.txt copied_out.txt",
    ], "move_and_copy")

    moved = os.path.join(work, "moved_out.txt")
    copied = os.path.join(work, "copied_out.txt")

    if not os.path.exists(moved) or "treasure" not in open(moved).read():
        results.fail("A moved file is still readable", f"transcript: {stdout!r}")
        return
    results.ok("A moved file is still readable")

    if not os.path.exists(copied) or "treasure" not in open(copied).read():
        results.fail("A copied file is still readable", "the copy did not decrypt")
        return
    results.ok("A copied file is still readable")

    entries = env.dek_manifest("carol")
    if "secret.txt" in entries or "renamed.txt" not in entries or "duplicate.txt" not in entries:
        results.fail("Keys follow MOVE and COPY", f"entries: {sorted(entries)}")
        return
    results.ok("Keys follow MOVE and COPY")


def test_delete_and_rmdir_drop_the_keys(env, results):
    env.run_user("carol", "pw_carol", "DELETE duplicate.txt", "delete_copy")

    if "duplicate.txt" in env.dek_manifest("carol"):
        results.fail("DELETE drops the key", "the manifest still has an entry for the deleted file")
        return
    results.ok("DELETE drops the key")

    tree = os.path.join(env.log_dir, "rmdir_tree")
    os.makedirs(tree, exist_ok=True)
    with open(os.path.join(tree, "inside.txt"), "w") as f:
        f.write("nested")

    env.run_user("carol", "pw_carol", f"UPLOAD_DIR {tree} doomed", "upload_doomed")
    if "doomed/inside.txt" not in env.dek_manifest("carol"):
        results.fail("RMDIR drops the subtree's keys", "the uploaded directory has no manifest entry")
        return

    env.run_user("carol", "pw_carol", "RMDIR doomed", "rmdir_doomed")
    remaining = [k for k in env.dek_manifest("carol") if k.startswith("doomed")]
    if remaining:
        results.fail("RMDIR drops the subtree's keys", f"entries left behind: {remaining}")
        return
    results.ok("RMDIR drops the subtree's keys")


def test_device_enrollment_and_revocation(env, results):
    env.with_vault("dave", "pw_dave", "dave")

    # One working directory for the whole test: the device's private keys are stored there, and
    # the point of enrolling is that the *next* login can use them.
    home = os.path.join(env.log_dir, "dave_home")
    os.makedirs(home, exist_ok=True)

    stdout, _ = env.run_user("dave", "pw_dave", ["ENROLL_DEVICE laptop", "DEVICES"], "enroll", cwd=home)

    # Device keys are hybrid X25519+ML-KEM-768, and ML-KEM needs OpenSSL 3.5+. Against an older
    # library the client refuses to enroll rather than quietly handing back a classical-only key,
    # so - like the tls suite's hybrid-group test - this asserts whichever of the two applies.
    refused = "ML-KEM-768" in (stdout or "") or "no OpenSSL" in (stdout or "")
    if refused:
        with open(env.vault_json("dave")) as f:
            if json.load(f).get("devices"):
                results.fail("Enrollment refused without ML-KEM",
                             "the client refused but a device was still enrolled on the server")
                return
        results.ok("Enrollment refused, not downgraded, where OpenSSL has no ML-KEM-768")

        # The password path is the one that must keep working on such a build
        local = make_local(env, "dave.txt", "password unlocked this")
        env.run_user("dave", "pw_dave", f"UPLOAD {local} dave.txt", "no_mlkem_upload", cwd=home)
        stdout, _ = env.run_user("dave", "pw_dave", "DOWNLOAD dave.txt dave_back.txt",
                                 "no_mlkem_download", cwd=home)
        back = os.path.join(home, "dave_back.txt")
        if not os.path.exists(back) or open(back).read().strip() != "password unlocked this":
            results.fail("Password unlock works without ML-KEM", f"transcript: {stdout!r}")
            return
        results.ok("Password unlock works without ML-KEM")
        return

    if "enrolled" not in (stdout or "").lower():
        results.fail("A device can be enrolled", f"transcript: {stdout!r}")
        return
    if "x25519+mlkem768" not in (stdout or ""):
        results.fail("A device can be enrolled", "the hybrid algorithm was not reported")
        return
    results.ok("A device can be enrolled")

    with open(env.vault_json("dave")) as f:
        devices = json.load(f).get("devices", [])
    if len(devices) != 1 or not devices[0].get("wrapped_vk_device", {}).get("kem_ct"):
        results.fail("Enrollment stores a hybrid sealed key", f"devices: {devices}")
        return
    # The server must hold only public halves and a sealed blob
    if any(k in json.dumps(devices) for k in ("x25519_sec", "mlkem_sec", "priv")):
        results.fail("Enrollment stores a hybrid sealed key", "a private key reached the server")
        return
    results.ok("Enrollment stores a hybrid sealed key")

    local = make_local(env, "dave.txt", "device unlocked this")
    stdout, _ = env.run_user("dave", "pw_dave", [f"UPLOAD {local} dave.txt", "VAULT_STATUS"],
                             "device_unlock", cwd=home)

    if "unlocked with this device's key" not in (stdout or ""):
        results.fail("An enrolled device unlocks the vault", f"transcript: {stdout!r}")
        return
    results.ok("An enrolled device unlocks the vault")

    device_id = devices[0]["device_id"]
    stdout, _ = env.run_user("dave", "pw_dave", [f"REVOKE_DEVICE {device_id}", "y"],
                             "revoke", cwd=home)

    with open(env.vault_json("dave")) as f:
        if json.load(f).get("devices"):
            results.fail("A device can be revoked", "the device is still enrolled")
            return
    results.ok("A device can be revoked")

    # Falling back to the password has to keep working, and the file stays readable
    stdout, work = env.run_user("dave", "pw_dave", ["DOWNLOAD dave.txt dave_back.txt", "VAULT_STATUS"],
                                "after_revoke", cwd=home)
    back = os.path.join(home, "dave_back.txt")
    if not os.path.exists(back) or open(back).read().strip() != "device unlocked this":
        results.fail("A revoked device falls back to the password", f"transcript: {stdout!r}")
        return
    if "unlocked with this device's key" in (stdout or ""):
        results.fail("A revoked device falls back to the password", "the revoked device still unlocked")
        return
    results.ok("A revoked device falls back to the password")


def test_devices_requires_a_vault(env, results):
    env.register("nick", "pw_nick", "register_nick")
    stdout, _ = env.run_user("nick", "pw_nick", "DEVICES", "devices_without_vault")

    if 412 not in error_codes(stdout):
        results.fail("DEVICES needs a vault", f"expected 412, got {error_codes(stdout)}")
        return
    results.ok("DEVICES needs a vault")


def test_plain_account_is_unaffected(env, results):
    """An account that never runs VAULT_INIT behaves exactly as it always has."""
    env.register("pete", "pw_pete", "register_pete")

    local = make_local(env, "plain.txt", "stored in the clear, as before")
    env.run_user("pete", "pw_pete", f"UPLOAD {local} plain.txt", "upload_plain")

    stored = os.path.join(env.files_dir("pete"), "plain.txt")
    if not os.path.exists(stored):
        results.fail("A plain account still stores plaintext", "nothing was stored")
        return
    with open(stored) as f:
        if f.read() != "stored in the clear, as before":
            results.fail("A plain account still stores plaintext", "the stored bytes changed")
            return
    results.ok("A plain account still stores plaintext")

    if os.path.exists(env.vault_json("pete")):
        results.fail("A plain account has no vault", "vault.json was created without VAULT_INIT")
        return
    results.ok("A plain account has no vault")

    stdout, work = env.run_user("pete", "pw_pete", "DOWNLOAD plain.txt plain_back.txt", "download_plain")
    back = os.path.join(work, "plain_back.txt")
    if not os.path.exists(back) or open(back).read() != "stored in the clear, as before":
        results.fail("A plain account round trips", f"transcript: {stdout!r}")
        return
    results.ok("A plain account round trips")


def test_files_predating_the_vault_stay_readable(env, results):
    """Turning the vault on must not strand what is already stored."""
    env.register("olive", "pw_olive", "register_olive")

    before = make_local(env, "before.txt", "uploaded before the vault existed")
    env.run_user("olive", "pw_olive", f"UPLOAD {before} before.txt", "upload_before_vault")
    env.run_user("olive", "pw_olive", "VAULT_INIT", "olive_vault_init")

    after = make_local(env, "after.txt", "uploaded after the vault existed")
    stdout, work = env.run_user("olive", "pw_olive", [
        f"UPLOAD {after} after.txt",
        "DOWNLOAD before.txt before_back.txt",
        "DOWNLOAD after.txt after_back.txt",
    ], "mixed_account")

    before_back = os.path.join(work, "before_back.txt")
    after_back = os.path.join(work, "after_back.txt")

    if not os.path.exists(before_back) or open(before_back).read() != "uploaded before the vault existed":
        results.fail("A file predating the vault is still readable", f"transcript: {stdout!r}")
        return
    results.ok("A file predating the vault is still readable")

    if not os.path.exists(after_back) or open(after_back).read() != "uploaded after the vault existed":
        results.fail("A file added after the vault is readable", "the new file did not come back")
        return
    results.ok("A file added after the vault is readable")

    entries = env.dek_manifest("olive")
    if "before.txt" in entries or "after.txt" not in entries:
        results.fail("Only files added after the vault are sealed", f"entries: {sorted(entries)}")
        return
    results.ok("Only files added after the vault are sealed")


def main():
    print("MiniDrive Integration Tests - End-to-End Encryption (vault)")
    print("=" * 60)

    check_executables()

    env = VaultTestEnvironment("vault", SERVER_PORT)
    results = TestResult(env.log_dir)

    try:
        env.setup_server_root()
        env.start_server()

        test_vault_init_creates_key_material(env, results)
        test_second_vault_init_is_refused(env, results)
        test_public_mode_has_no_vault(env, results)
        test_server_stores_ciphertext_only(env, results)
        test_round_trip_is_byte_identical(env, results)
        test_manifest_records_keys_not_content(env, results)
        test_tampered_ciphertext_is_refused(env, results)
        test_other_user_cannot_read_the_file(env, results)
        test_sync_is_idempotent_for_a_vaulted_account(env, results)
        test_download_dir_reconstructs_the_tree(env, results)
        test_move_and_copy_carry_the_key(env, results)
        test_delete_and_rmdir_drop_the_keys(env, results)
        test_device_enrollment_and_revocation(env, results)
        test_devices_requires_a_vault(env, results)
        test_plain_account_is_unaffected(env, results)
        test_files_predating_the_vault_stay_readable(env, results)

    finally:
        env.cleanup()

    ok = results.summary()
    print(f"\nLogs saved to: {env.log_dir}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
