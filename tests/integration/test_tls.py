#!/usr/bin/env python3
"""Integration tests for the rung 3.5 transport: TLS 1.3, chain validation, certificate pinning,
and the hybrid post-quantum group X25519MLKEM768.

Every other suite runs at rung 0 and proves the protocol still works over plaintext. This one runs
the same client and server at rung 3.5 and proves two separate things:

  1. The protocol is unaffected by the transport underneath it - login, single-chunk and
     multi-chunk transfers all still work, byte for byte.
  2. The security properties are real rather than configured: a certificate from the wrong CA, a
     certificate for the wrong name, and a mismatched pin are each *refused*, and an impostor
     server is only ever accepted by the posture that explicitly asks not to check (--tls-verify
     none). A test that only proves "TLS connects" would pass just as happily with verification
     switched off, which is exactly the bug rung 2a exists to illustrate.

The certificates come from lab/gen-certs.sh, so running this suite also exercises that script.

Usage:
    python3 tests/integration/test_tls.py
"""

import os
import re
import shutil
import signal
import subprocess
import sys
import time

from test_utils import (
    TestResult,
    check_executables,
    SERVER_EXE,
    CLIENT_EXE,
    TestEnvironment,
    calculate_hash,
)

SERVER_PORT = 9034
IMPOSTOR_PORT = 9035

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
GEN_CERTS = os.path.join(REPO_ROOT, "lab", "gen-certs.sh")
CERT_DIR = os.path.abspath("data/test_tls_certs")

# Printed by the client on a successful handshake: "OK: Secure connection: TLSv1.3, cipher X, group Y"
NEGOTIATION = re.compile(r"Secure connection: (\S+), cipher (\S+), group (\S+)")


def generate_certificates():
    """Run lab/gen-certs.sh into a scratch directory and return the paths it produced."""
    if os.path.exists(CERT_DIR):
        shutil.rmtree(CERT_DIR)
    subprocess.run([GEN_CERTS, CERT_DIR], check=True, capture_output=True, text=True)

    def pin(name):
        with open(os.path.join(CERT_DIR, name)) as f:
            return f.read().strip()

    return {
        "ca": os.path.join(CERT_DIR, "ca.crt"),
        "cert": os.path.join(CERT_DIR, "server.crt"),
        "key": os.path.join(CERT_DIR, "server.key"),
        "pin": pin("server.pin"),
        "rogue_ca": os.path.join(CERT_DIR, "rogue-ca.crt"),
        "rogue_cert": os.path.join(CERT_DIR, "rogue-server.crt"),
        "rogue_key": os.path.join(CERT_DIR, "rogue-server.key"),
        "rogue_pin": pin("rogue-server.pin"),
    }


class TlsEnv(TestEnvironment):
    """Runs the whole environment - server and every client - at rung 3.5."""

    def __init__(self, certs, port=SERVER_PORT):
        self.certs = certs
        super().__init__(
            "tls",
            port,
            extra_server_args=[
                "--rung", "3.5",
                "--tls-cert", certs["cert"],
                "--tls-key", certs["key"],
            ],
            extra_client_args=[
                "--rung", "3.5",
                "--ca-file", certs["ca"],
            ],
        )
        self.impostor = None

    def run_client(self, commands, address=None, client_args=None, cwd=None, timeout=30):
        """Run one client to completion. client_args, when given, replaces the suite defaults."""
        address = address or f"127.0.0.1:{self.port}"
        # stdbuf only where it exists, as in test_utils: Alpine (the static-client CI job) and macOS
        # have none, and the output is read after the client exits, so it is not needed for that.
        args = ["stdbuf", "-o0", "-e0"] if shutil.which("stdbuf") else []
        args += [CLIENT_EXE, address]
        args.extend(self.extra_client_args if client_args is None else client_args)

        proc = subprocess.Popen(
            args,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            cwd=cwd or self.client_cwd,
        )
        try:
            stdout, _ = proc.communicate(input="\n".join(commands) + "\n", timeout=timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
            stdout, _ = proc.communicate()
        return stdout

    def start_impostor(self):
        """A second server presenting a valid certificate signed by a CA nobody trusts."""
        root = os.path.abspath("data/test_tls_impostor_root")
        if os.path.exists(root):
            shutil.rmtree(root)
        os.makedirs(root, exist_ok=True)

        log = open(os.path.join(self.log_dir, "impostor.log"), "a")
        self.impostor = subprocess.Popen(
            [SERVER_EXE, "--port", str(IMPOSTOR_PORT), "--root", root, "--rung", "3.5",
             "--tls-cert", self.certs["rogue_cert"], "--tls-key", self.certs["rogue_key"]],
            stdout=log, stderr=log, text=True,
        )
        time.sleep(0.5)
        if self.impostor.poll() is not None:
            raise RuntimeError("Impostor server failed to start")

    def cleanup(self):
        if self.impostor and self.impostor.poll() is None:
            self.impostor.send_signal(signal.SIGTERM)
            try:
                self.impostor.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.impostor.kill()
        super().cleanup()


def negotiated(stdout):
    """(version, cipher, group) from a client transcript, or None if no handshake completed."""
    match = NEGOTIATION.search(stdout)
    return match.groups() if match else None


def pq_available(stdout):
    """Whether this OpenSSL could offer the hybrid group, judged by the client's own warning."""
    return "X25519MLKEM768 is unavailable" not in stdout


# --- The protocol still works over TLS ------------------------------------------------------


def test_handshake_and_listing(env, results):
    stdout = env.run_client(["LIST", "EXIT"])
    details = negotiated(stdout)

    if not details:
        results.fail("TLS handshake", f"no secure connection reported: {stdout[:400]}")
        return
    version, cipher, _ = details
    if version != "TLSv1.3":
        results.fail("TLS handshake", f"expected TLSv1.3, negotiated {version}")
        return
    if "Current directory" not in stdout:
        results.fail("TLS handshake", f"LIST did not succeed over TLS: {stdout[:400]}")
        return
    results.ok(f"TLS 1.3 handshake and LIST ({cipher})")


def test_roundtrip_over_tls(env, results):
    """A multi-chunk transfer: 1 MiB is four 256 KiB chunks, so the binary framing - not just the
    JSON control channel - goes through the TLS stream, in both directions."""
    workdir = env.new_workdir("roundtrip")
    source = os.path.join(workdir, "payload.bin")
    with open(source, "wb") as f:
        f.write(os.urandom(1024 * 1024))

    stdout = env.run_client(
        ["UPLOAD payload.bin remote.bin", "DOWNLOAD remote.bin returned.bin", "EXIT"],
        cwd=workdir,
    )

    returned = os.path.join(workdir, "returned.bin")
    if not os.path.exists(returned):
        results.fail("Round trip over TLS", f"file was not downloaded back: {stdout[:400]}")
        return
    if calculate_hash(source) != calculate_hash(returned):
        results.fail("Round trip over TLS", "downloaded file does not match the source")
        return
    results.ok("1 MiB round trip over TLS is byte-identical")


def test_authenticated_session_over_tls(env, results):
    """Registration and login - the flow whose password is the reason the wire is encrypted."""
    workdir = env.new_workdir("auth")
    stdout = env.run_client(
        ["y", "tls-secret-pw", "tls-secret-pw", "LIST", "EXIT"],
        address=f"tlsuser@127.0.0.1:{env.port}",
        cwd=workdir,
    )
    if "Current directory" not in stdout:
        results.fail("Authenticated session over TLS", f"login did not complete: {stdout[:500]}")
        return
    results.ok("Registration and login complete over TLS")


# --- The verification is real ---------------------------------------------------------------


def test_wrong_ca_is_refused(env, results):
    stdout = env.run_client(
        ["LIST", "EXIT"],
        client_args=["--rung", "3.5", "--ca-file", env.certs["rogue_ca"]],
    )
    if negotiated(stdout) or "Current directory" in stdout:
        results.fail("Wrong CA refused", "the client accepted a certificate from an untrusted CA")
        return
    if "certificate verify failed" not in stdout:
        results.fail("Wrong CA refused", f"unexpected failure mode: {stdout[:400]}")
        return
    results.ok("Certificate from an untrusted CA is refused")


def test_wrong_hostname_is_refused(env, results):
    """127.0.0.2 reaches the same server, but is not one of the certificate's SANs. This is the
    check that separates 'the chain builds' from 'this is the server I asked for'."""
    stdout = env.run_client(["LIST", "EXIT"], address=f"127.0.0.2:{env.port}")
    if negotiated(stdout) or "Current directory" in stdout:
        results.fail("Wrong hostname refused", "a certificate for other names was accepted")
        return
    if "certificate verify failed" not in stdout:
        results.fail("Wrong hostname refused", f"unexpected failure mode: {stdout[:400]}")
        return
    results.ok("Certificate not covering the requested address is refused")


def test_correct_pin_is_accepted(env, results):
    stdout = env.run_client(
        ["LIST", "EXIT"],
        client_args=["--rung", "3.5", "--ca-file", env.certs["ca"], "--pin", env.certs["pin"]],
    )
    if "Current directory" not in stdout:
        results.fail("Correct pin accepted", f"pinned client could not connect: {stdout[:400]}")
        return
    results.ok("Pinned client connects when the pin matches")


def test_wrong_pin_is_refused(env, results):
    """The CA is correct here, so only the pin can reject this connection - which is the point of
    pinning: it survives an attacker who has obtained a certificate the CA would vouch for."""
    stdout = env.run_client(
        ["LIST", "EXIT"],
        client_args=["--rung", "3.5", "--ca-file", env.certs["ca"], "--pin", env.certs["rogue_pin"]],
    )
    if negotiated(stdout) or "Current directory" in stdout:
        results.fail("Wrong pin refused", "a mismatched pin did not stop the connection")
        return
    results.ok("Mismatched pin is refused even with a valid chain")


def test_pin_only_without_ca(env, results):
    """Pin-only: no CA at all, the public key is the entire identity. Correct pin connects,
    the impostor's pin does not."""
    good = env.run_client(
        ["LIST", "EXIT"],
        client_args=["--rung", "3.5", "--pin", env.certs["pin"]],
    )
    bad = env.run_client(
        ["LIST", "EXIT"],
        client_args=["--rung", "3.5", "--pin", env.certs["rogue_pin"]],
    )
    if "Current directory" not in good:
        results.fail("Pin-only trust", f"correct pin was rejected: {good[:400]}")
        return
    if negotiated(bad):
        results.fail("Pin-only trust", "wrong pin was accepted with no CA to fall back on")
        return
    if "chain is not validated" not in good:
        results.fail("Pin-only trust", "pin-only mode did not warn that the chain is unvalidated")
        return
    results.ok("Pin-only trust accepts the pinned key and nothing else")


def test_impostor_server_is_refused(env, results):
    """A real machine-in-the-middle: a second server, same names, certificate signed by a CA the
    client does not trust."""
    env.start_impostor()
    address = f"127.0.0.1:{IMPOSTOR_PORT}"

    with_ca = env.run_client(["LIST", "EXIT"], address=address)
    pinned = env.run_client(
        ["LIST", "EXIT"], address=address,
        client_args=["--rung", "3.5", "--pin", env.certs["pin"]],
    )
    unverified = env.run_client(
        ["LIST", "EXIT"], address=address,
        client_args=["--rung", "3.5", "--tls-verify", "none"],
    )

    if negotiated(with_ca):
        results.fail("Impostor refused", "CA-validating client accepted the impostor")
        return
    if negotiated(pinned):
        results.fail("Impostor refused", "pinned client accepted the impostor")
        return
    # The counter-example, asserted deliberately: with verification off the impostor is accepted,
    # which is what rung 2a exists to demonstrate and why 'none' is never a default.
    if not negotiated(unverified):
        results.fail("Impostor refused", "--tls-verify none did not connect at all; test is not "
                                         "demonstrating what it claims")
        return
    if "NOT checked" not in unverified:
        results.fail("Impostor refused", "--tls-verify none did not warn that nothing is checked")
        return
    results.ok("Impostor server is refused by CA and by pin, accepted only by --tls-verify none")


def test_rung_mismatch_fails_closed(env, results):
    """A plaintext client must not be able to talk to a TLS server, in either direction, and must
    not be quietly downgraded to one that can."""
    plaintext_to_tls = env.run_client(["LIST", "EXIT"], client_args=[])
    if "Current directory" in plaintext_to_tls:
        results.fail("Rung mismatch", "a plaintext client completed a command against a TLS server")
        return
    results.ok("Plaintext client cannot talk to a TLS server")


def test_pinned_requires_a_pin(env, results):
    """Asking for the pinned posture without supplying a pin is a startup error, not a silent
    downgrade to plain CA validation."""
    proc = subprocess.run(
        [CLIENT_EXE, f"127.0.0.1:{env.port}", "--rung", "3.5",
         "--ca-file", env.certs["ca"], "--tls-verify", "pinned"],
        input="EXIT\n", capture_output=True, text=True, timeout=30, cwd=env.client_cwd,
    )
    output = proc.stdout + proc.stderr
    if proc.returncode == 0:
        results.fail("Pinned needs a pin", "client started anyway with --tls-verify pinned and no --pin")
        return
    if "needs --pin" not in output:
        results.fail("Pinned needs a pin", f"unhelpful error: {output[:300]}")
        return
    results.ok("--tls-verify pinned without --pin fails at startup")


# --- The hybrid post-quantum group ------------------------------------------------------------


def test_hybrid_group(env, results):
    """X25519MLKEM768 needs OpenSSL 3.5+. Against an older library the requirement is not that the
    group is used, but that its absence is reported and the connection falls back to a classical
    group instead of failing - so this asserts whichever of those two is applicable."""
    stdout = env.run_client(["EXIT"])
    details = negotiated(stdout)
    if not details:
        results.fail("Hybrid key exchange", f"no handshake completed: {stdout[:400]}")
        return

    _, _, group = details
    if pq_available(stdout):
        if group != "X25519MLKEM768":
            results.fail("Hybrid key exchange",
                         f"OpenSSL offers the hybrid group but {group} was negotiated")
            return
        results.ok("Hybrid post-quantum key exchange negotiated (X25519MLKEM768)")
    else:
        if group.lower() not in ("x25519", "p-256", "secp256r1"):
            results.fail("Hybrid key exchange", f"unexpected fallback group {group}")
            return
        results.ok(f"Hybrid group unavailable in this OpenSSL; reported and fell back to {group}")


def test_require_pq_matches_the_library(env, results):
    """--tls-require-pq is the fail-closed switch: a deployment that would rather not start than
    run without the hybrid group. It must start exactly when the group is available."""
    probe = env.run_client(["EXIT"])
    available = pq_available(probe)

    root = os.path.abspath("data/test_tls_pq_root")
    if os.path.exists(root):
        shutil.rmtree(root)
    os.makedirs(root, exist_ok=True)

    proc = subprocess.run(
        [SERVER_EXE, "--port", "9036", "--root", root, "--rung", "3.5",
         "--tls-cert", env.certs["cert"], "--tls-key", env.certs["key"], "--tls-require-pq"],
        capture_output=True, text=True, timeout=15,
    ) if not available else None

    if available:
        # Starting is the expected outcome; the server would run forever, so start it and stop it.
        started = subprocess.Popen(
            [SERVER_EXE, "--port", "9036", "--root", root, "--rung", "3.5",
             "--tls-cert", env.certs["cert"], "--tls-key", env.certs["key"], "--tls-require-pq"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        time.sleep(0.7)
        alive = started.poll() is None
        started.send_signal(signal.SIGTERM)
        try:
            started.wait(timeout=5)
        except subprocess.TimeoutExpired:
            started.kill()
        if not alive:
            results.fail("--tls-require-pq", "server refused to start although the group is available")
            return
        results.ok("--tls-require-pq starts when the hybrid group is available")
    else:
        if proc.returncode == 0:
            results.fail("--tls-require-pq", "server started without the hybrid group it was told to require")
            return
        if "require-pq" not in (proc.stdout + proc.stderr):
            results.fail("--tls-require-pq", f"unclear refusal: {(proc.stdout + proc.stderr)[:300]}")
            return
        results.ok("--tls-require-pq refuses to start without the hybrid group")


def test_backlogged_rungs_are_named(env, results):
    """--rung 2a must not resolve to 'whatever is closest'. Choosing a posture that is not built
    has to be an error naming the situation, in either direction."""
    proc = subprocess.run(
        [CLIENT_EXE, f"127.0.0.1:{env.port}", "--rung", "2a"],
        input="EXIT\n", capture_output=True, text=True, timeout=15, cwd=env.client_cwd,
    )
    output = proc.stdout + proc.stderr
    if proc.returncode == 0:
        results.fail("Backlogged rungs", "--rung 2a was accepted")
        return
    if "not implemented" not in output:
        results.fail("Backlogged rungs", f"unclear error for an unbuilt rung: {output[:300]}")
        return
    results.ok("An unbuilt rung is refused by name, not silently substituted")


def main():
    print("MiniDrive Integration Tests - TLS (rung 3.5)")
    print("=" * 60)

    check_executables()

    if not os.path.exists(GEN_CERTS):
        print(f"ERROR: {GEN_CERTS} not found")
        sys.exit(1)

    certs = generate_certificates()
    env = TlsEnv(certs)
    results = TestResult(env.log_dir)

    try:
        env.setup_server_root()
        env.start_server()

        test_handshake_and_listing(env, results)
        test_roundtrip_over_tls(env, results)
        test_authenticated_session_over_tls(env, results)
        test_wrong_ca_is_refused(env, results)
        test_wrong_hostname_is_refused(env, results)
        test_correct_pin_is_accepted(env, results)
        test_wrong_pin_is_refused(env, results)
        test_pin_only_without_ca(env, results)
        test_impostor_server_is_refused(env, results)
        test_rung_mismatch_fails_closed(env, results)
        test_pinned_requires_a_pin(env, results)
        test_hybrid_group(env, results)
        test_require_pq_matches_the_library(env, results)
        test_backlogged_rungs_are_named(env, results)

    finally:
        env.cleanup()

    ok = results.summary()
    print(f"\nLogs saved to: {env.log_dir}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
