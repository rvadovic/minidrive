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

Added with the desktop GUI (step 11), which renders these rather than parsing terminal text:
- LIST, TIERS, DEVICES, VAULT_STATUS and batches emit structured EVENTs, and a sealed file is
  listed at the size its owner stored, not the ciphertext's.
- Quoted arguments carry paths with spaces through every command a file manager issues.
- A confirm PROMPT carries the question it answers, and keeps it across a re-ask.
- --state-dir moves the client's bookkeeping out of the working directory.

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

    def __init__(self, env, label, endpoint="127.0.0.1", timeout=20, extra_args=None):
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
            extra_args=["--ipc", self.sock_path] + (extra_args or []),
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

    def mark(self):
        """Position in the transcript, so a later wait only considers frames after it."""
        with self.frames_lock:
            return len(self.frames)

    def wait_for(self, predicate, timeout=20, since=0):
        """Waits for a frame matching `predicate`. Returns it, or None on timeout."""
        deadline = time.time() + timeout
        seen = since
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

    def event_named(self, name, predicate=lambda e: True, timeout=20):
        return self.wait_for(
            lambda f: f.get("type") == "EVENT" and f.get("event") == name and predicate(f), timeout)

    def result_containing(self, text, timeout=20):
        return self.wait_for(lambda f: f.get("type") == "RESULT" and text in f.get("message", ""), timeout)

    def login(self, password, register=False):
        """Answers the registration question if asked, then the password prompt."""
        if register:
            self.wait_for(lambda f: f.get("type") == "PROMPT" and f.get("kind") == "confirm")
            self.send_line("y")
        self.wait_for(lambda f: f.get("type") == "PROMPT" and f.get("kind") == "password")
        self.send_line(password)
        return self.wait_for(lambda f: f.get("type") == "PROMPT" and f.get("kind") == "command", timeout=30)

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


def test_listing_is_structured_and_quoted_paths_work(env, results):
    """The GUI draws LIST from the listing event, and addresses everything by quoted path."""
    name = "LIST emits structured entries; quoted paths with spaces work"
    client = IpcClient(env, "listing")
    try:
        client.wait_for(lambda f: f.get("type") == "PROMPT")

        source = os.path.join(client.cwd, "My Source.bin")
        with open(source, "wb") as f:
            f.write(os.urandom(300 * 1024 + 7))

        client.send_line('MKDIR "/struct dir"')
        client.result_containing("Directory created")
        client.send_line(f'UPLOAD "{source}" "/struct dir/My File.bin"')
        uploaded = client.result_containing("Upload successful", timeout=40)

        client.send_line('LIST "/struct dir"')
        inner = client.event_named("listing", lambda e: e.get("path") == "/struct dir")
        client.send_line('LIST "/"')
        outer = client.event_named("listing", lambda e: e.get("path") == "/")

        fetched = os.path.join(client.cwd, "fetched back.bin")
        client.send_line(f'DOWNLOAD "/struct dir/My File.bin" "{fetched}"')
        downloaded = client.result_containing("Download successful", timeout=40)
        client.send_line("EXIT")
        client.wait_exit()

        up_progress = [e for e in client.events()
                       if e.get("event") == "transfer_progress" and e.get("direction") == "upload"]
        inner_entries = {e["name"]: e for e in (inner or {}).get("entries", [])}
        outer_entries = {e["name"]: e for e in (outer or {}).get("entries", [])}

        if uploaded is None:
            results.fail(name, f"quoted UPLOAD did not complete: {client.transcript()[-600:]}")
        elif inner is None or outer is None:
            results.fail(name, f"no listing event: {client.transcript()[-600:]}")
        elif "My File.bin" not in inner_entries:
            results.fail(name, f"entry missing from listing: {inner}")
        elif inner_entries["My File.bin"].get("is_directory") is not False \
                or inner_entries["My File.bin"].get("size") != 300 * 1024 + 7:
            results.fail(name, f"entry has the wrong type or size: {inner_entries['My File.bin']}")
        elif abs(inner_entries["My File.bin"].get("last_modified", 0) - time.time()) > 24 * 3600:
            # Unix seconds. Before the fix this was the file clock's raw count, which libstdc++
            # measures from 2174 - negative for any real file, wrapped to ~1.8e19.
            results.fail(name, f"modification time is not a recent Unix time: {inner_entries['My File.bin'].get('last_modified')}")
        elif outer_entries.get("struct dir", {}).get("is_directory") is not True:
            results.fail(name, f"directory not listed as one: {outer}")
        elif not up_progress or up_progress[-1].get("remote_path") != "/struct dir/My File.bin" \
                or up_progress[-1].get("local_path") != os.path.abspath(source):
            results.fail(name, f"progress does not name the transfer: {up_progress[-1:] }")
        elif downloaded is None or not os.path.exists(fetched) or md5(fetched) != md5(source):
            results.fail(name, "quoted DOWNLOAD did not return the same bytes")
        else:
            results.ok(name)
    finally:
        client.cleanup()


def test_confirm_prompt_carries_question(env, results):
    name = "Confirm PROMPT carries its question, across a re-ask"
    client = IpcClient(env, "question", endpoint=f"ipcquestion@127.0.0.1:{env.port}")
    try:
        first = client.wait_for(lambda f: f.get("type") == "PROMPT" and f.get("kind") == "confirm")
        mark = client.mark()
        client.send_line("maybe")  # not y/n: the client re-asks
        again = client.wait_for(
            lambda f: f.get("type") == "PROMPT" and f.get("kind") == "confirm", since=mark)
        client.send_line("y")
        client.wait_for(lambda f: f.get("type") == "PROMPT" and f.get("kind") == "password")
        client.send_line("questionpass123")
        client.wait_for(lambda f: f.get("type") == "PROMPT" and f.get("kind") == "command", timeout=30)
        client.send_line("EXIT")
        client.wait_exit()

        if first is None or not first.get("text"):
            results.fail(name, f"confirm PROMPT has no question: {first}")
        elif not any(first["text"] == r.get("message") for r in client.results()):
            results.fail(name, f"question is not the server's message: {first['text']!r}")
        elif again is None or again.get("text") != first.get("text"):
            results.fail(name, f"re-ask lost the question: {again}")
        else:
            results.ok(name)
    finally:
        client.cleanup()


def test_batch_events(env, results):
    name = "Batches emit per-item and summary events"
    client = IpcClient(env, "batchevents")
    try:
        client.wait_for(lambda f: f.get("type") == "PROMPT")
        tree = os.path.join(client.cwd, "evtree")
        os.makedirs(os.path.join(tree, "sub"), exist_ok=True)
        for rel in ["one.txt", "sub/two.txt"]:
            with open(os.path.join(tree, rel), "w") as f:
                f.write("x" * 100)

        client.send_line(f'UPLOAD_DIR "{tree}" /ipc_evtree')
        summary = client.event_named("batch_summary", timeout=40)
        client.send_line("EXIT")
        client.wait_exit()

        items = [e for e in client.events() if e.get("event") == "batch_item"]
        uploads = [e for e in items if e.get("op") == "upload"]
        if summary is None:
            results.fail(name, f"no batch_summary event: {client.transcript()[-600:]}")
        elif summary.get("uploaded") != 2 or summary.get("failed") != 0:
            results.fail(name, f"summary counts are wrong: {summary}")
        elif not items or items[-1].get("index") != items[-1].get("total") or items[0].get("index") != 1:
            results.fail(name, f"batch_item indices are wrong: {items}")
        elif sorted(e.get("remote_path") for e in uploads) != ["/ipc_evtree/one.txt", "/ipc_evtree/sub/two.txt"]:
            results.fail(name, f"upload items do not name their paths: {uploads}")
        else:
            results.ok(name)
    finally:
        client.cleanup()


def test_state_dir(env, results):
    name = "--state-dir moves the client's bookkeeping"
    state = os.path.join(tempfile.mkdtemp(prefix="mdipc_state_"), "accounts", "public@here")
    client = IpcClient(env, "statedir", extra_args=["--state-dir", state])
    try:
        client.wait_for(lambda f: f.get("type") == "PROMPT")
        client.send_line("EXIT")
        rc = client.wait_exit()

        if rc != 0:
            results.fail(name, f"client exited with {rc}")
        elif not os.path.isfile(os.path.join(state, ".partial", "partmeta.json")):
            results.fail(name, f"no partial-transfer database under {state}")
        elif os.path.exists(os.path.join(client.cwd, "data", "client_cwd")):
            results.fail(name, "the working directory still got a data/client_cwd")
        else:
            results.ok(name)
    finally:
        client.cleanup()
        shutil.rmtree(os.path.dirname(os.path.dirname(state)), ignore_errors=True)


def test_account_events(env, results):
    """TIERS, VAULT_STATUS and DEVICES as events, and a sealed file listed at its plaintext size."""
    name = "Tier, vault and device events; sealed files listed at plaintext size"
    client = IpcClient(env, "account", endpoint=f"ipcvault@127.0.0.1:{env.port}", timeout=60)
    try:
        if client.login("vaultpass123", register=True) is None:
            results.fail(name, f"login failed: {client.transcript()[-600:]}")
            return

        client.send_line("TIERS")
        tiers = client.event_named("tiers")
        client.send_line("VAULT_STATUS")
        before = client.event_named("vault_status")
        mark = client.mark()
        client.send_line("VAULT_INIT")
        client.wait_for(lambda f: f.get("type") == "PROMPT", timeout=30, since=mark)
        client.send_line("VAULT_STATUS")
        after = client.event_named("vault_status", lambda e: e.get("enabled") is True, timeout=30)
        client.send_line("DEVICES")
        devices = client.event_named("devices")

        source = os.path.join(client.cwd, "secret.bin")
        with open(source, "wb") as f:
            f.write(os.urandom(600 * 1024))  # three chunks, so ciphertext is visibly larger
        client.send_line(f'UPLOAD "{source}" /secret.bin')
        client.result_containing("Upload successful", timeout=40)
        client.send_line('LIST "/"')
        listing = client.event_named("listing", lambda e: e.get("path") == "/")
        client.send_line("EXIT")
        client.wait_exit()

        entries = {e["name"]: e for e in (listing or {}).get("entries", [])}
        stored = os.path.join(env.server_root, "private", "ipcvault", "files", "secret.bin")

        if tiers is None or not any(t.get("current") for t in tiers.get("tiers", [])):
            results.fail(name, f"no tiers event with a current tier: {tiers}")
        elif before is None or before.get("enabled") is not False or before.get("unlocked") is not False:
            results.fail(name, f"vault_status before VAULT_INIT is wrong: {before}")
        elif after is None or after.get("unlocked") is not True:
            results.fail(name, f"vault_status after VAULT_INIT is wrong: {after}")
        elif devices is None or devices.get("devices") != []:
            results.fail(name, f"devices event is wrong: {devices}")
        elif "secret.bin" not in entries:
            results.fail(name, f"sealed file missing from the listing: {listing}")
        elif entries["secret.bin"].get("size") != 600 * 1024:
            results.fail(name, f"sealed file listed at {entries['secret.bin'].get('size')}, not its plaintext size")
        elif not os.path.exists(stored) or os.path.getsize(stored) == 600 * 1024:
            results.fail(name, "the server copy is not the (larger) ciphertext - the check proves nothing")
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
        test_listing_is_structured_and_quoted_paths_work(env, results)
        test_confirm_prompt_carries_question(env, results)
        test_batch_events(env, results)
        test_state_dir(env, results)
        test_account_events(env, results)
        test_host_disconnect_exits_cleanly(env, results)
        test_oversized_frame_is_refused(env, results)
        test_missing_socket_is_a_startup_failure(env, results)
    finally:
        env.stop_server()
    sys.exit(0 if results.summary() else 1)


if __name__ == "__main__":
    main()
