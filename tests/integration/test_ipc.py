#!/usr/bin/env python3
"""Integration tests for headless --ipc mode (suite id `ipc`).

The client's state machine, protocol, SYNC/batch/resume engines and vault crypto are the same in
headless mode; only the console changes - commands arrive as length-prefixed JSON frames over a
Unix domain socket instead of off a terminal, and results/progress go back the same way. This
suite drives the client the way the desktop GUI's Rust core will: it creates the socket, spawns
the client against it, and speaks frames.

What is asserted, and why each one is here rather than just "IPC connects":
- Frames are the wire protocol's own framing (4-byte big-endian length + JSON body), so a host has
  one framing to implement, not two.
- A PROMPT frame is emitted exactly when input is armed, and names what is wanted: command,
  password, or a y/n confirm. That is the only way a GUI can know which control to show.
- Registration and password entry work over the channel, so headless mode is not read-only.
- Transfers emit throttled progress EVENTs and the stored file matches its source byte for byte -
  the events are not a substitute for the transfer actually working.
- A command queued behind a running transfer does not preempt it. This is Known Bug #1's property,
  and it has to hold for a queued channel exactly as it does for buffered stdin.
- The host closing the socket ends the client cleanly, rather than leaving an orphan holding a
  server connection open.
- A bogus frame length is refused rather than turned into an allocation.
- Pointing --ipc at nothing is a startup failure with a message, not a silent hang.

Usage:
    python3 tests/integration/test_ipc.py
"""

import hashlib
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

from test_utils import (
    TestResult,
    TestEnvironment,
    check_executables,
    CLIENT_EXE,
)

SERVER_PORT = 9050


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


class IpcClient:
    """One headless client, plus the socket the host end of it speaks over.

    The host owns the socket and the client connects to it, which is the arrangement the real
    sidecar uses: socket lifetime and cleanup stay with the supervising process.
    """

    def __init__(self, env, label, endpoint="127.0.0.1", timeout=20):
        self.env = env
        self.label = label
        self.frames = []
        self.frames_lock = threading.Lock()
        self.closed = threading.Event()

        self.dir = tempfile.mkdtemp(prefix=f"mdipc_{label}_")
        self.sock_path = os.path.join(self.dir, "ipc.sock")
        self.cwd = env.new_workdir(label)

        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.listener.bind(self.sock_path)
        self.listener.listen(1)
        self.listener.settimeout(timeout)

        self.process = env.start_client_process(
            endpoint if ":" in endpoint else f"{endpoint}:{env.port}",
            os.path.join(env.log_dir, f"{label}_client.log"),
            cwd=self.cwd,
            extra_args=["--ipc", self.sock_path],
        )

        self.conn, _ = self.listener.accept()
        self.conn.settimeout(timeout)
        self.reader = threading.Thread(target=self._pump, daemon=True)
        self.reader.start()

    # -- framing -------------------------------------------------------------
    def send_line(self, line):
        body = json.dumps({"type": "COMMAND", "line": line}).encode()
        self.conn.sendall(struct.pack(">I", len(body)) + body)

    def send_raw(self, payload):
        self.conn.sendall(payload)

    def _recv_exact(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.conn.recv(n - len(buf))
            if not chunk:
                return None
            buf += chunk
        return buf

    def _pump(self):
        try:
            while True:
                header = self._recv_exact(4)
                if header is None:
                    break
                length = struct.unpack(">I", header)[0]
                body = self._recv_exact(length)
                if body is None:
                    break
                with self.frames_lock:
                    self.frames.append(json.loads(body))
        except Exception:
            pass
        finally:
            self.closed.set()

    # -- waiting -------------------------------------------------------------
    def snapshot(self):
        with self.frames_lock:
            return list(self.frames)

    def wait_for(self, predicate, timeout=20):
        """Waits for a frame matching `predicate`. Returns it, or None on timeout."""
        deadline = time.time() + timeout
        seen = 0
        while time.time() < deadline:
            frames = self.snapshot()
            for frame in frames[seen:]:
                if predicate(frame):
                    return frame
            seen = len(frames)
            time.sleep(0.02)
        return None

    def results(self):
        return [f for f in self.snapshot() if f.get("type") == "RESULT"]

    def events(self):
        return [f for f in self.snapshot() if f.get("type") == "EVENT"]

    def prompts(self):
        return [f for f in self.snapshot() if f.get("type") == "PROMPT"]

    def transcript(self):
        return "\n".join(json.dumps(f) for f in self.snapshot())

    # -- teardown ------------------------------------------------------------
    def close_host(self):
        """Drop the socket from the host side without sending EXIT."""
        try:
            self.conn.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.conn.close()

    def wait_exit(self, timeout=10):
        try:
            return self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
            return None

    def cleanup(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait()
        try:
            self.conn.close()
        except OSError:
            pass
        self.listener.close()
        shutil.rmtree(self.dir, ignore_errors=True)


class IpcEnv(TestEnvironment):
    def __init__(self):
        super().__init__("ipc", SERVER_PORT)
        # The harness normally probes for --log support by running a client on stdin. A headless
        # build has no stdin mode and would fail that probe, so the answer is given directly -
        # this suite is run against both builds and --log is supported by each.
        self.supports_logging = True

    def public_file(self, name):
        return os.path.join(self.server_root, "public", "files", name)


def test_command_round_trip(env, results):
    name = "Command round trip over the socket"
    client = IpcClient(env, "roundtrip")
    try:
        # The public-mode banner arrives unprompted, then the client says it is idle.
        banner = client.wait_for(lambda f: f.get("type") == "RESULT")
        prompt = client.wait_for(lambda f: f.get("type") == "PROMPT")
        client.send_line("MKDIR ipcdir")
        made = client.wait_for(lambda f: f.get("type") == "RESULT" and "Directory created" in f.get("message", ""))
        client.send_line("LIST")
        listing = client.wait_for(lambda f: f.get("type") == "RESULT" and "ipcdir" in f.get("message", ""))
        client.send_line("EXIT")
        rc = client.wait_exit()

        if banner is None:
            results.fail(name, "no RESULT frame after connecting")
        elif prompt is None or prompt.get("kind") != "command":
            results.fail(name, f"expected a command PROMPT, got {prompt}")
        elif made is None:
            results.fail(name, f"MKDIR was not acknowledged: {client.transcript()[-400:]}")
        elif listing is None:
            results.fail(name, f"LIST did not show the new directory: {client.transcript()[-400:]}")
        elif rc != 0:
            results.fail(name, f"client exited with {rc}")
        else:
            results.ok(name)
    finally:
        client.cleanup()


def test_result_frames_are_shaped(env, results):
    name = "RESULT frames carry ok/code/message"
    client = IpcClient(env, "shape")
    try:
        client.wait_for(lambda f: f.get("type") == "PROMPT")
        client.send_line("DELETE definitely_not_here.txt")
        failure = client.wait_for(lambda f: f.get("type") == "RESULT" and f.get("ok") is False)
        client.send_line("EXIT")
        client.wait_exit()

        if failure is None:
            results.fail(name, f"no failing RESULT frame: {client.transcript()[-400:]}")
        elif not isinstance(failure.get("code"), int) or failure["code"] == 200:
            results.fail(name, f"error frame has no error code: {failure}")
        elif not failure.get("message"):
            results.fail(name, f"error frame has no message: {failure}")
        else:
            results.ok(name)
    finally:
        client.cleanup()


def test_registration_and_password_prompt(env, results):
    name = "Registration and password entry over IPC"
    client = IpcClient(env, "register", endpoint=f"ipcuser@127.0.0.1:{env.port}")
    try:
        # The server asks whether to register; the client must say it wants a y/n, not a command.
        confirm = client.wait_for(lambda f: f.get("type") == "PROMPT" and f.get("kind") == "confirm")
        client.send_line("y")
        password = client.wait_for(lambda f: f.get("type") == "PROMPT" and f.get("kind") == "password")
        client.send_line("ipcpassword123")
        # First login registers the account, so the success line names registration, not auth.
        authed = client.wait_for(
            lambda f: f.get("type") == "RESULT" and f.get("ok")
            and ("Registration successful" in f.get("message", "")
                 or "uthenticat" in f.get("message", "")))
        client.send_line("EXIT")
        rc = client.wait_exit()

        if confirm is None:
            results.fail(name, f"no confirm PROMPT for the registration question: {client.transcript()[-400:]}")
        elif password is None:
            results.fail(name, f"no password PROMPT: {client.transcript()[-400:]}")
        elif "ipcuser" not in password.get("text", ""):
            results.fail(name, f"password PROMPT does not name the account: {password}")
        elif authed is None:
            results.fail(name, f"authentication did not succeed: {client.transcript()[-600:]}")
        elif rc != 0:
            results.fail(name, f"client exited with {rc}")
        else:
            results.ok(name)
    finally:
        client.cleanup()


def test_password_never_echoed(env, results):
    name = "Password is not echoed back over the channel"
    client = IpcClient(env, "nopwecho", endpoint=f"ipcuser@127.0.0.1:{env.port}")
    try:
        client.wait_for(lambda f: f.get("type") == "PROMPT" and f.get("kind") == "password")
        client.send_line("ipcpassword123")
        client.wait_for(lambda f: f.get("type") == "RESULT" and "uthenticat" in f.get("message", ""))
        client.send_line("EXIT")
        client.wait_exit()

        transcript = client.transcript()
        if "ipcpassword123" in transcript:
            results.fail(name, "the password appears in a frame sent back to the host")
        else:
            results.ok(name)
    finally:
        client.cleanup()


def test_upload_progress_and_integrity(env, results):
    name = "Upload emits progress and stores the real bytes"
    client = IpcClient(env, "upload")
    try:
        client.wait_for(lambda f: f.get("type") == "PROMPT")

        source = os.path.join(client.cwd, "payload.bin")
        with open(source, "wb") as f:
            f.write(os.urandom(900 * 1024))  # several chunks, so progress has something to report

        client.send_line("UPLOAD payload.bin ipc_upload.bin")
        done = client.wait_for(
            lambda f: f.get("type") == "RESULT" and "Upload successful" in f.get("message", ""), timeout=40)
        client.send_line("EXIT")
        client.wait_exit()

        progress = [e for e in client.events() if e.get("event") == "transfer_progress"]
        stored = env.public_file("ipc_upload.bin")

        if done is None:
            results.fail(name, f"upload never completed: {client.transcript()[-600:]}")
        elif not progress:
            results.fail(name, "no transfer_progress events were emitted")
        elif progress[0].get("chunks_done") != 0:
            results.fail(name, f"first progress event is not at zero: {progress[0]}")
        elif progress[-1].get("chunks_done") != progress[-1].get("chunks_total"):
            results.fail(name, f"last progress event is not complete: {progress[-1]}")
        elif any(e.get("direction") != "upload" for e in progress):
            results.fail(name, "a progress event reported the wrong direction")
        elif not os.path.exists(stored):
            results.fail(name, f"server has no file at {stored}")
        elif md5(stored) != md5(source):
            results.fail(name, "the stored file does not match its source")
        else:
            results.ok(name)
    finally:
        client.cleanup()


def test_download_round_trip(env, results):
    name = "Download over IPC returns the same bytes"
    client = IpcClient(env, "download")
    try:
        client.wait_for(lambda f: f.get("type") == "PROMPT")

        source = os.path.join(client.cwd, "down_src.bin")
        with open(source, "wb") as f:
            f.write(os.urandom(700 * 1024))

        client.send_line("UPLOAD down_src.bin ipc_download.bin")
        client.wait_for(lambda f: f.get("type") == "RESULT" and "Upload successful" in f.get("message", ""),
                        timeout=40)
        client.send_line("DOWNLOAD ipc_download.bin fetched.bin")
        done = client.wait_for(
            lambda f: f.get("type") == "RESULT" and "Download successful" in f.get("message", ""), timeout=40)
        client.send_line("EXIT")
        client.wait_exit()

        fetched = os.path.join(client.cwd, "files", "fetched.bin")
        if not os.path.exists(fetched):
            fetched = os.path.join(client.cwd, "fetched.bin")

        downloads = [e for e in client.events()
                     if e.get("event") == "transfer_progress" and e.get("direction") == "download"]

        if done is None:
            results.fail(name, f"download never completed: {client.transcript()[-600:]}")
        elif not downloads:
            results.fail(name, "no download progress events were emitted")
        elif not os.path.exists(fetched):
            results.fail(name, f"no downloaded file under {client.cwd}")
        elif md5(fetched) != md5(source):
            results.fail(name, "the downloaded file does not match the original")
        else:
            results.ok(name)
    finally:
        client.cleanup()


def test_queued_command_does_not_preempt_transfer(env, results):
    """Known Bug #1's property, for a queued channel.

    A host that pushes UPLOAD and EXIT back to back must not have the EXIT abort the transfer: a
    line is only handed over when the client asks for one, exactly as buffered stdin behaves.
    """
    name = "A queued EXIT does not preempt a running transfer"
    client = IpcClient(env, "queued")
    try:
        client.wait_for(lambda f: f.get("type") == "PROMPT")

        source = os.path.join(client.cwd, "queued.bin")
        with open(source, "wb") as f:
            f.write(os.urandom(1200 * 1024))

        client.send_line("UPLOAD queued.bin ipc_queued.bin")
        client.send_line("EXIT")  # pushed immediately, while the transfer is starting

        done = client.wait_for(
            lambda f: f.get("type") == "RESULT" and "Upload successful" in f.get("message", ""), timeout=40)
        rc = client.wait_exit()
        stored = env.public_file("ipc_queued.bin")

        if done is None:
            results.fail(name, f"the transfer was interrupted: {client.transcript()[-600:]}")
        elif rc != 0:
            results.fail(name, f"client exited with {rc}")
        elif not os.path.exists(stored) or md5(stored) != md5(source):
            results.fail(name, "the queued EXIT cost the transfer its file")
        else:
            results.ok(name)
    finally:
        client.cleanup()


def test_host_disconnect_exits_cleanly(env, results):
    name = "Host closing the socket ends the client cleanly"
    client = IpcClient(env, "disconnect")
    try:
        client.wait_for(lambda f: f.get("type") == "PROMPT")
        client.close_host()
        rc = client.wait_exit()

        if rc is None:
            results.fail(name, "client did not exit after the host disconnected")
        elif rc != 0:
            results.fail(name, f"client exited with {rc} rather than cleanly")
        else:
            results.ok(name)
    finally:
        client.cleanup()


def test_oversized_frame_is_refused(env, results):
    """Four host-chosen bytes must not become an allocation.

    Same bound the wire protocol applies (transport::MAX_MESSAGE_SIZE); the point is that the
    channel refuses and closes rather than trying to honour it.
    """
    name = "An out-of-range frame length is refused"
    client = IpcClient(env, "badframe")
    try:
        client.wait_for(lambda f: f.get("type") == "PROMPT")
        client.send_raw(struct.pack(">I", 0xFFFFFFFF))  # ~4 GiB, well past the cap
        rc = client.wait_exit()

        if rc is None:
            results.fail(name, "client neither refused the frame nor exited")
        elif rc != 0:
            results.fail(name, f"client exited with {rc} rather than cleanly")
        else:
            results.ok(name)
    finally:
        client.cleanup()


def test_missing_socket_is_a_startup_failure(env, results):
    name = "--ipc pointing at nothing fails at startup"
    missing = os.path.join(tempfile.gettempdir(), "minidrive_no_such_ipc.sock")
    if os.path.exists(missing):
        os.remove(missing)

    process = subprocess.Popen(
        [CLIENT_EXE, f"127.0.0.1:{env.port}", "--ipc", missing],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, cwd=env.client_cwd)
    try:
        output, _ = process.communicate(timeout=10)
        rc = process.returncode
    except subprocess.TimeoutExpired:
        process.kill()
        output, _ = process.communicate()
        rc = None

    if rc is None:
        results.fail(name, "client hung instead of refusing to start")
    elif rc != 1:
        results.fail(name, f"expected rc=1, got {rc}: {output.strip()[-200:]}")
    elif missing not in output:
        results.fail(name, f"the message does not name the socket: {output.strip()[-200:]}")
    else:
        results.ok(name)


def test_batch_summary_reaches_the_host(env, results):
    """A batch is driven by the same engine in headless mode, so its summary must come back too."""
    name = "Directory upload reports its summary over IPC"
    client = IpcClient(env, "batch")
    try:
        client.wait_for(lambda f: f.get("type") == "PROMPT")

        tree = os.path.join(client.cwd, "tree")
        os.makedirs(os.path.join(tree, "nested"), exist_ok=True)
        for rel in ["a.txt", "nested/b.txt"]:
            with open(os.path.join(tree, rel), "w") as f:
                f.write("contents of " + rel)

        client.send_line("UPLOAD_DIR tree ipc_tree")
        summary = client.wait_for(
            lambda f: f.get("type") == "RESULT" and "complete." in f.get("message", "")
            and "Uploaded:" in f.get("message", ""), timeout=40)
        client.send_line("EXIT")
        client.wait_exit()

        uploaded_a = env.public_file(os.path.join("ipc_tree", "a.txt"))
        uploaded_b = env.public_file(os.path.join("ipc_tree", "nested", "b.txt"))

        if summary is None:
            results.fail(name, f"no batch summary frame: {client.transcript()[-600:]}")
        elif "Uploaded: 2" not in summary.get("message", ""):
            results.fail(name, f"summary does not report both files: {summary.get('message')}")
        elif not os.path.exists(uploaded_a) or not os.path.exists(uploaded_b):
            results.fail(name, "the uploaded tree is not on the server")
        else:
            results.ok(name)
    finally:
        client.cleanup()


def main():
    check_executables()
    env = IpcEnv()
    results = TestResult(env.log_dir)
    env.setup_server_root()
    env.start_server()
    try:
        print("Headless --ipc mode tests")
        test_command_round_trip(env, results)
        test_result_frames_are_shaped(env, results)
        test_registration_and_password_prompt(env, results)
        test_password_never_echoed(env, results)
        test_upload_progress_and_integrity(env, results)
        test_download_round_trip(env, results)
        test_queued_command_does_not_preempt_transfer(env, results)
        test_batch_summary_reaches_the_host(env, results)
        test_host_disconnect_exits_cleanly(env, results)
        test_oversized_frame_is_refused(env, results)
        test_missing_socket_is_a_startup_failure(env, results)
    finally:
        env.stop_server()
    sys.exit(0 if results.summary() else 1)


if __name__ == "__main__":
    main()
