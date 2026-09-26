#!/usr/bin/env python3
"""End-to-end test of the desktop GUI: the real app, the real client sidecar, a real server.

Drives the built Tauri app through WebDriver (tauri-driver + WebKitWebDriver) the way a user
would - typing into the connect form, answering the registration and password dialogs, browsing
folders, turning the vault on - against a server running at rung 3.5 with certificate pinning.

It is deliberately not "the window opens":
- the posture badge must show what TLS actually negotiated (post-quantum on OpenSSL >= 3.5),
- a folder seeded on the server's disk must appear, and navigating into it must list its contents,
- turning the vault on must leave the server holding ciphertext for a file uploaded afterwards,
- a wrong password must be reported in the password dialog, not silently re-asked,
- a wrong pin must be refused with the client's own reason on the connect screen.

Linux only (tauri-driver's WebKit backend). Requirements: a debug build of the app with the
frontend embedded (`npx tauri build --debug --no-bundle`), `tauri-driver` on PATH,
WebKitWebDriver (Debian: webkit2gtk-driver), Xvfb, openssl, and a built server.

Usage: python3 gui/tests/e2e.py [--app PATH] [--keep]
"""

import argparse
import base64
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
GUI = os.path.dirname(HERE)
REPO = os.path.dirname(GUI)
BUILD = os.environ.get("MINIDRIVE_BUILD_DIR", os.path.join(REPO, "build"))
SERVER = os.path.join(BUILD, "server", "server")
DEFAULT_APP = os.path.join(GUI, "src-tauri", "target", "debug", "minidrive-gui")
ELEMENT = "element-6066-11e4-a52f-4a8d6d8a4e65"


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_port(port, timeout=15):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.5).close()
            return
        except OSError:
            time.sleep(0.1)
    raise RuntimeError(f"nothing listening on {port}")


class Failure(Exception):
    pass


class Driver:
    """The handful of W3C WebDriver calls this test needs, over plain HTTP."""

    def __init__(self, port, shots):
        self.base = f"http://127.0.0.1:{port}"
        self.session = None
        self.shots = shots
        self.shot_no = 0

    def call(self, method, path, body=None):
        data = None if body is None else json.dumps(body).encode()
        request = urllib.request.Request(self.base + path, data=data, method=method,
                                         headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(request, timeout=120) as response:
                return json.loads(response.read() or b"{}").get("value")
        except urllib.error.HTTPError as e:
            detail = e.read().decode(errors="replace")
            raise Failure(f"WebDriver {method} {path}: {e.code} {detail[:300]}")

    def start(self, app):
        value = self.call("POST", "/session", {"capabilities": {"alwaysMatch": {"tauri:options": {"application": app}}}})
        self.session = value["sessionId"]

    def quit(self):
        if self.session:
            try:
                self.call("DELETE", f"/session/{self.session}")
            except Exception:
                pass

    def s(self, path):
        return f"/session/{self.session}{path}"

    def js(self, script, *args):
        return self.call("POST", self.s("/execute/sync"), {"script": script, "args": list(args)})

    def js_async(self, script, *args):
        return self.call("POST", self.s("/execute/async"), {"script": script, "args": list(args)})

    def find(self, css, timeout=20):
        deadline = time.time() + timeout
        while time.time() < deadline:
            found = self.call("POST", self.s("/elements"), {"using": "css selector", "value": css})
            if found:
                # W3C names the reference key ELEMENT; some drivers still use the legacy "ELEMENT".
                ref = found[0]
                return ref.get(ELEMENT) or ref.get("ELEMENT") or next(iter(ref.values()))
            time.sleep(0.15)
        self.screenshot(f"missing {css}")
        raise Failure(f"no element matches {css!r} after {timeout}s")

    def click(self, css, timeout=20):
        self.call("POST", self.s(f"/element/{self.find(css, timeout)}/click"), {})

    def type(self, css, text, clear=True):
        element = self.find(css)
        if clear:
            self.call("POST", self.s(f"/element/{element}/clear"), {})
        self.call("POST", self.s(f"/element/{element}/value"), {"text": text})

    def text(self, css):
        # textContent rather than WebDriver's element text: WebKitWebDriver leaves the file
        # manager's row labels out of the latter, although they are plainly on screen.
        self.find(css)
        return self.js("const e = document.querySelector(arguments[0]); return e ? e.textContent : '';", css)

    def wait_text(self, css, needle, timeout=30):
        deadline = time.time() + timeout
        last = ""
        while time.time() < deadline:
            try:
                last = self.text(css)
                if needle in last:
                    return last
            except Failure:
                pass
            time.sleep(0.2)
        self.screenshot(f"waiting for {needle}")
        raise Failure(f"{css!r} never contained {needle!r}; last text: {last[:400]!r}")

    def screenshot(self, name):
        try:
            png = self.call("GET", self.s("/screenshot"))
        except Exception:
            return
        self.shot_no += 1
        safe = "".join(c if c.isalnum() else "_" for c in name)[:60]
        with open(os.path.join(self.shots, f"{self.shot_no:02d}_{safe}.png"), "wb") as f:
            f.write(base64.b64decode(png))


def open_item(driver, name):
    """Opens a row in the file list the way a user does: two quick clicks on it. (The component
    detects the double click in its click handler, so a synthetic dblclick event does nothing.)"""
    element = driver.find(f'.file-item-container[title="{name}"]')
    for _ in range(2):
        driver.call("POST", driver.s(f"/element/{element}/click"), {})


def drop(driver, paths):
    """What an OS drag-and-drop onto the window delivers: Tauri's drag-drop event with real paths.
    Emitted from the page, so everything after it - inspecting the paths, UPLOAD vs UPLOAD_DIR,
    the replace question, the refresh - runs exactly as for a real drop."""
    driver.js_async(
        "const done = arguments[arguments.length - 1];"
        "window.__TAURI_INTERNALS__.invoke('plugin:event|emit', {event: 'tauri://drag-drop',"
        " payload: {paths: arguments[0], position: {x: 400, y: 300}}}).then(() => done(true), e => done(String(e)));",
        paths)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--app", default=DEFAULT_APP)
    parser.add_argument("--keep", action="store_true", help="keep the scratch directory")
    args = parser.parse_args()

    for path, what in [(args.app, "the GUI (npx tauri build --debug --no-bundle)"), (SERVER, "the server")]:
        if not os.path.isfile(path):
            print(f"missing {what}: {path}")
            return 1
    for tool in ["tauri-driver", "WebKitWebDriver", "Xvfb", "openssl"]:
        if not shutil.which(tool):
            print(f"missing tool: {tool}")
            return 1

    scratch = tempfile.mkdtemp(prefix="mdgui_e2e_")
    stamp = time.strftime("%Y%m%d_%H%M%S")
    shots = os.path.join(REPO, "test_logs", f"gui_e2e_{stamp}")
    os.makedirs(shots, exist_ok=True)
    procs = []
    passed, failed = [], []

    def check(name, fn):
        try:
            fn()
            passed.append(name)
            print(f"  ✓ {name}")
        except Exception as e:  # noqa: BLE001 - every failure is reported, then the run continues
            failed.append(name)
            print(f"  ✗ {name}: {e}")
            driver.screenshot(f"FAIL {name}")

    try:
        certs = os.path.join(scratch, "certs")
        subprocess.run(["bash", os.path.join(REPO, "lab", "gen-certs.sh"), certs], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        pin = open(os.path.join(certs, "server.pin")).read().strip()
        rogue_pin = open(os.path.join(certs, "rogue-server.pin")).read().strip()

        # Two tiers so the Storage view has a choice; files seeded straight onto the server's disk
        # so the file manager has something to show that the GUI did not put there itself.
        root = os.path.join(scratch, "root")
        archive = os.path.join(scratch, "archive")
        os.makedirs(os.path.join(root, "private", "e2e_user", "files", "Photos"))
        os.makedirs(archive)
        with open(os.path.join(root, "private", "e2e_user", "files", "Quarterly Report.pdf"), "wb") as f:
            f.write(os.urandom(300 * 1024))
        with open(os.path.join(root, "private", "e2e_user", "files", "Photos", "beach day.jpg"), "wb") as f:
            f.write(os.urandom(64 * 1024))

        port = free_port()
        procs.append(subprocess.Popen(
            [SERVER, "--port", str(port), "--root", root, "--rung", "3.5",
             "--tls-cert", os.path.join(certs, "server.crt"), "--tls-key", os.path.join(certs, "server.key"),
             "--tier", f"hot={root}", "--tier", f"archive={archive}", "--tier-desc", "archive=Spinning disks",
             "--default-tier", "hot"],
            stdout=open(os.path.join(shots, "server.log"), "w"), stderr=subprocess.STDOUT))
        wait_port(port)

        display = ":93"
        procs.append(subprocess.Popen(["Xvfb", display, "-screen", "0", "1280x860x24"],
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        time.sleep(1)

        home = os.path.join(scratch, "home")
        os.makedirs(home)
        env = dict(os.environ, DISPLAY=display, HOME=home, GDK_BACKEND="x11", WEBKIT_DISABLE_DMABUF_RENDERER="1",
                   XDG_CONFIG_HOME=os.path.join(home, ".config"), XDG_DATA_HOME=os.path.join(home, ".local", "share"))
        env.pop("WAYLAND_DISPLAY", None)  # a forwarded Wayland socket would take the window instead
        driver_port = free_port()
        procs.append(subprocess.Popen(["tauri-driver", "--port", str(driver_port)], env=env,
                                      stdout=open(os.path.join(shots, "tauri-driver.log"), "w"), stderr=subprocess.STDOUT))
        wait_port(driver_port)

        driver = Driver(driver_port, shots)
        driver.start(args.app)
        print("Desktop GUI end-to-end")

        def sign_up():
            driver.find("[data-testid=connect]")
            driver.type("[data-testid=profile-name]", "E2E NAS")
            driver.type("[data-testid=profile-host]", "127.0.0.1")
            driver.type("[data-testid=profile-port]", str(port))
            driver.type("[data-testid=profile-username]", "e2e_user")
            driver.type("[data-testid=profile-pin]", pin)
            driver.screenshot("connect form")
            driver.click("[data-testid=connect]")
            question = driver.wait_text("[data-testid=question-text]", "register")
            assert "(Y/n)" not in question, "the dialog should offer buttons, not (Y/n)"
            driver.click("[data-testid=question-yes]")
            driver.type("[data-testid=password-input]", "e2e password 1")
            driver.click("[data-testid=password-submit]")
            driver.wait_text("[data-testid=account]", "e2e_user@127.0.0.1")

        check("Register through the connect form and dialogs", sign_up)

        def posture():
            badge = driver.text("[data-testid=posture]")
            version = subprocess.run(["openssl", "version"], capture_output=True, text=True).stdout
            major_minor = tuple(int(x) for x in version.split()[1].split(".")[:2])
            expected = "post-quantum" if major_minor >= (3, 5) else "classical"
            assert "TLS 1.3" in badge and expected in badge, f"badge says {badge!r} with {version.strip()}"

        check("Header shows the negotiated TLS posture", posture)

        def browse():
            driver.wait_text("[data-testid=file-manager]", "Quarterly Report.pdf")
            driver.wait_text("[data-testid=file-manager]", "Photos")
            driver.screenshot("files root")
            open_item(driver, "Photos")
            driver.wait_text("[data-testid=file-manager]", "beach day.jpg")
            driver.screenshot("files photos")

        check("Files: seeded folders listed, and a folder opens", browse)

        def upload_by_drop():
            note = os.path.join(scratch, "drop note.txt")
            with open(note, "w") as f:
                f.write("first version\n" * 100)
            album = os.path.join(scratch, "Album")
            os.makedirs(album)
            for name in ["one.jpg", "two.jpg"]:
                with open(os.path.join(album, name), "wb") as f:
                    f.write(os.urandom(40 * 1024))
            files = os.path.join(root, "private", "e2e_user", "files", "Photos")

            drop(driver, [note, album])  # into the folder being shown: /Photos
            driver.wait_text("[data-testid=file-manager]", "drop note.txt", timeout=60)
            driver.wait_text("[data-testid=file-manager]", "Album")
            assert open(os.path.join(files, "drop note.txt")).read() == open(note).read()
            assert sorted(os.listdir(os.path.join(files, "Album"))) == ["one.jpg", "two.jpg"], "folder drop did not upload the tree"
            driver.screenshot("after drop")

            with open(note, "w") as f:
                f.write("second version\n" * 100)
            drop(driver, [note])
            driver.wait_text(".modal", "already exists")
            driver.screenshot("replace question")
            driver.click(".modal .btn-danger")
            # Replacing is DELETE then UPLOAD, so the file is briefly absent in between.
            stored = os.path.join(files, "drop note.txt")
            deadline = time.time() + 30
            while time.time() < deadline and not (os.path.exists(stored) and open(stored).read() == open(note).read()):
                time.sleep(0.2)
            assert os.path.exists(stored) and open(stored).read() == open(note).read(), "replace did not replace"

        check("Drop to upload: files, folders, and replacing after asking", upload_by_drop)

        def vault():
            driver.click("[data-testid=tab-vault]")
            driver.click("[data-testid=vault-enable]")
            driver.click(".modal .btn-primary")  # the GUI's own "no recovery" confirmation
            driver.wait_text(".content", "Unlocked", timeout=60)
            driver.screenshot("vault on")
            # A file uploaded now must reach the server sealed.
            note = os.path.join(scratch, "secret note.txt")
            with open(note, "w") as f:
                f.write("the plaintext the server must never see\n" * 50)
            result = driver.js_async(
                "const done = arguments[arguments.length - 1];"
                "window.__TAURI_INTERNALS__.invoke('execute', {op: {op: 'upload', local: arguments[0], remote: '/secret note.txt'}})"
                ".then(r => done(r), e => done({error: String(e)}));",
                note)
            assert "error" not in result, result
            stored = os.path.join(root, "private", "e2e_user", "files", "secret note.txt")
            data = open(stored, "rb").read()
            assert b"plaintext the server must never see" not in data, "the server holds plaintext"

        check("Vault: turned on from the GUI, uploads are sealed", vault)

        def storage():
            driver.click("[data-testid=tab-storage]")
            driver.wait_text(".content", "Spinning disks")
            driver.wait_text(".content", "current")
            driver.screenshot("storage")

        check("Storage: tiers listed with the current one marked", storage)

        def activity():
            driver.click("[data-testid=tab-activity]")
            driver.wait_text("[data-testid=activity-log]", "Vault created")
            # Folder visits are LISTs; their text rendering must not flood the log.
            assert "Quarterly Report.pdf" not in driver.text("[data-testid=activity-log]"), "listings reached the log"
            log = driver.text("[data-testid=activity-log]")
            assert "e2e password 1" not in log, "the password reached the activity log"
            driver.screenshot("activity")

        check("Activity: the session's results are logged, the password is not", activity)

        def reconnect_with_wrong_password():
            driver.click("[data-testid=disconnect]")
            driver.find("[data-testid=connect]")
            driver.click("[data-testid=connect]")
            driver.type("[data-testid=password-input]", "not my password")
            driver.click("[data-testid=password-submit]")
            driver.wait_text(".modal", "Invalid password")
            driver.screenshot("wrong password")
            driver.type("[data-testid=password-input]", "e2e password 1")
            driver.click("[data-testid=password-submit]")
            driver.wait_text("[data-testid=account]", "e2e_user@127.0.0.1")
            driver.wait_text(".top-bar", "End-to-end encrypted")

        check("Wrong password is shown in the dialog; the right one unlocks the vault", reconnect_with_wrong_password)

        def wrong_pin():
            driver.click("[data-testid=disconnect]")
            driver.type("[data-testid=profile-pin]", rogue_pin)
            driver.click("[data-testid=connect]")
            driver.wait_text("[data-testid=connect-error]", "TLS handshake failed")
            driver.screenshot("wrong pin")

        check("A wrong pin is refused with the client's reason", wrong_pin)

        def cancel_hanging_connect():
            # Accepts the TCP connection (the kernel does, into the backlog) and never answers.
            silent = socket.socket()
            silent.bind(("127.0.0.1", 0))
            silent.listen(8)
            try:
                driver.type("[data-testid=profile-port]", str(silent.getsockname()[1]))
                driver.click("[data-testid=connect]")
                driver.click("[data-testid=cancel-connect]")
                deadline = time.time() + 15
                while time.time() < deadline and driver.js("return document.querySelector('[data-testid=connect]').disabled;"):
                    time.sleep(0.2)
                assert not driver.js("return document.querySelector('[data-testid=connect]').disabled;"), "still connecting"
                assert driver.text("[data-testid=connect]") == "Connect"
                assert not driver.js("return !!document.querySelector('[data-testid=connect-error]');"), \
                    "a cancel the user asked for is not an error"
            finally:
                silent.close()

        check("A hanging connect can be cancelled", cancel_hanging_connect)
    except Exception as e:  # noqa: BLE001
        failed.append(f"setup: {e}")
        print(f"  ✗ setup: {e}")
    finally:
        try:
            driver.quit()  # noqa: F821 - may not exist if setup failed early
        except Exception:
            pass
        for p in reversed(procs):
            p.terminate()
            try:
                p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                p.kill()
        if not args.keep:
            shutil.rmtree(scratch, ignore_errors=True)

    print(f"Results: {len(passed)}/{len(passed) + len(failed)} passed  (screenshots: {shots})")
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
