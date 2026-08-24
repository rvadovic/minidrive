# Transport security (rung 3.5)

MiniDrive runs at one of two postures, chosen with `--rung` on both the server and the client:

| `--rung` | What it is |
|---|---|
| `0` (default) | Plaintext TCP. Everything, including the password, travels in the clear. |
| `3.5` | TLS 1.3, chain validation, certificate pinning, and hybrid post-quantum key exchange. |

Rung 0 is the default because it is the baseline the whole integration suite runs on. Turning on
TLS is an explicit act with certificates behind it. There is no automatic upgrade and no automatic
downgrade: a rung 0 client cannot talk to a rung 3.5 server, or the other way round, and neither
side quietly falls back to the other's posture.

The intermediate rungs of the security ladder (1, 2a–2d) and mutual TLS (rung 4) are not built.
Passing one of their names is an error that says so, rather than resolving to the nearest posture
that does exist.

## Generating certificates

```sh
lab/gen-certs.sh                                   # writes to lab/certs/
SERVER_SANS="DNS:nas.example.com,IP:10.0.0.5" lab/gen-certs.sh
```

This produces a root CA, a server certificate signed by it, and the server's public key **pin**. It
also produces a *rogue* CA and a rogue server certificate — an impostor for testing that clients
really do refuse a certificate from an untrusted CA.

Every name or address a client may use to reach the server has to be in `SERVER_SANS`. At rung 3.5
the client checks the name it dialled against the certificate's SANs, so a missing name is a refused
connection, not a warning.

The `.key` files are the server's identity. They are generated per deployment, never committed, and
never distributed. Only `ca.crt` and the pin are meant to reach clients.

## Running

```sh
server --port 9000 --root /srv/minidrive --rung 3.5 \
       --tls-cert lab/certs/server.crt --tls-key lab/certs/server.key

client user@nas.example.com:9000 --rung 3.5 \
       --ca-file lab/certs/ca.crt --pin sha256:<hex>
```

The server prints its own certificate pin at startup, which is where the client's `--pin` value
comes from. On a successful handshake the client prints what was actually negotiated:

```
OK: Secure connection: TLSv1.3, cipher TLS_AES_256_GCM_SHA384, group X25519MLKEM768
```

That line is the proof of posture — a connection that failed validation never reaches it.

## How the client decides to trust the server

`--tls-verify` picks one of three postures. Supplying `--pin` selects `pinned` on its own.

| Mode | Meaning |
|---|---|
| `ca` (default) | The chain must validate to a trusted CA (`--ca-file`, or the system trust store), **and** the certificate must cover the name or address that was dialled. |
| `pinned` | Everything `ca` does, plus the leaf's public key must match `--pin`. With no `--ca-file`, the pin alone identifies the server and the chain is deliberately not validated — the client says so on every connection. |
| `none` | No checking at all. The connection is encrypted and trivially machine-in-the-middled. Never a default, and always warned about. |

**Pinning is over the public key**, not the certificate: SHA-256 of the DER-encoded
SubjectPublicKeyInfo. Renewing the certificate with the same key leaves every deployed pin valid.
The same value can be computed by hand:

```sh
openssl x509 -in server.crt -pubkey -noout \
  | openssl pkey -pubin -outform der \
  | openssl dgst -sha256
```

Pinning is what survives an attacker who has obtained a certificate that a CA *would* vouch for —
a rogue or compromised CA, or one injected into a corporate trust store. Chain validation alone
does not.

## Post-quantum key exchange

The default group list is `X25519MLKEM768:X25519:P-256` — the hybrid group first. `X25519MLKEM768`
combines the classical X25519 exchange with ML-KEM-768, so the session key stays secret unless
*both* are broken. It answers a threat that is happening now: an attacker who records traffic today
and decrypts it once a quantum computer exists ("harvest now, decrypt later"). That attacker is
passive, which is also why client certificates would not help against them.

`X25519MLKEM768` requires **OpenSSL 3.5 or newer**, which ships ML-KEM natively (no liboqs). On an
older OpenSSL the group is dropped from the list at startup, with a warning naming the version
found, and the handshake falls back to a classical group — the connection still happens and is
still TLS 1.3. `--tls-require-pq` turns that degradation into a startup failure instead, for a
deployment that would rather not run than run without it.

Both sides negotiate independently, so a new client and an old server (or vice versa) simply agree
on the best group they share.

**What matters is the OpenSSL on the machine running the binary, not the one it was built
against.** `libssl` is linked dynamically, and group names are resolved at runtime, so a release
package built on a distribution with OpenSSL 3.0 still negotiates `X25519MLKEM768` when it runs on
a system with 3.5 — and reports it as such. Upgrading the host's OpenSSL is enough; nothing needs
rebuilding.

## What this does and does not protect

TLS protects the wire: the password, filenames, and file contents are unreadable and unmodifiable
in transit, and the client knows it is talking to the real server.

It does not protect anything **from the server**. The server still sees every file in plaintext.
Making the server zero-knowledge for file contents is the vault (end-to-end encryption), a separate
piece of work that sits on top of this one — and one that would be pointless underneath a plaintext
wire, since its key material travels in the login response.

## Other flags

| Flag | Side | Purpose |
|---|---|---|
| `--tls-groups <list>` | both | Override the key-exchange group preference list. |
| `--tls-require-pq` | both | Refuse to start if the hybrid group is unavailable. |
| `--tls-min-version <1.2\|1.3>` | both | Minimum protocol version. Defaults to 1.3; 1.2 exists for the backlogged downgrade rungs. |
| `--tls-ciphers <list>` | both | Override the cipher suites. |
| `--tls-servername <name>` | client | Check the certificate against this name instead of the one dialled — for reaching a server by IP whose certificate carries only a DNS name. |

## Testing

`tests/integration/test_tls.py` (suite id `tls`) runs the whole environment at rung 3.5. It covers
the protocol working over TLS — including a multi-chunk transfer and a full login — and, separately,
that the verification is real: a certificate from the wrong CA, one for the wrong name, and a
mismatched pin are each refused, and an impostor server is accepted only by `--tls-verify none`.

```sh
python3 tests/integration/run_all_tests.py --suite tls
```

The two post-quantum tests adapt to the OpenSSL they find: against 3.5+ they require the hybrid
group to be negotiated, and against anything older they require the fallback to be reported rather
than silent.
