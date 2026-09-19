# End-to-end encryption (the vault)

TLS protects the wire. The vault protects the data from the server itself: with a vault turned on,
the MiniDrive server stores file **contents** it cannot read, and holds no key that would let it.

The two are orthogonal and both worth having. TLS stops someone on the network; the vault stops
someone with the disks, the backups, or root on the server. Turning the vault on without TLS is
close to pointless — the salt, the Argon2id parameters and the sealed vault key travel in the login
response, so anyone who can watch one login can derive the master key exactly as the real client
does. Run `--rung 3.5` (see `docs/tls.md`) and the vault together.

The vault is **opt-in per account**. An account that never runs `VAULT_INIT` behaves exactly as it
always has, byte for byte.

## Using it

```
alice@nas:~$ client alice@nas.example.com:9000 --rung 3.5 --ca-file ca.crt --pin sha256:...
Password for alice:
OK: Authentication successful.
> VAULT_INIT
Deriving your vault key (this takes a moment)...
OK: Vault created. Files uploaded from now on are encrypted before they leave this machine.
> UPLOAD ./taxes.pdf taxes.pdf
OK: Upload successful
```

| Command | What it does |
|---|---|
| `VAULT_INIT` | Turns on end-to-end encryption for this account. Once, and it cannot be undone. |
| `VAULT_STATUS` | Reports locally whether the vault exists, is unlocked, and whether this device is enrolled. |
| `ENROLL_DEVICE <name>` | Generates this machine's hybrid key pair and seals the vault key to it. |
| `DEVICES` | Lists the devices enrolled in the vault. |
| `REVOKE_DEVICE <id>` | Removes one, after a confirmation. |

Everything else is unchanged. `UPLOAD`, `DOWNLOAD`, `SYNC`, `UPLOAD_DIR`, `DOWNLOAD_DIR`, batch
`MOVE`/`COPY`/`DELETE` and resumable transfers all work the same way on a vaulted account, because
encryption sits underneath them rather than beside them.

## The key hierarchy

```
password ──Argon2id(vault_salt, opslimit=3, memlimit=256 MiB)──► MK ──unwraps──► VK
                                                                                 │
  device private keys ──X25519 + ML-KEM-768 decapsulation──────────► VK          │ unwraps
                                                                                 ▼
                                                            per-file DEK ──seals──► chunks
```

| Key | Where it is generated | Where it lives | Sealed by |
|---|---|---|---|
| Password | the user | never stored | — |
| **MK** (master key) | client, from the password | client RAM, for as long as one unlock takes | — |
| **VK** (vault key) | client, once, at `VAULT_INIT` | client RAM while unlocked; the server keeps only sealed copies | MK, and each enrolled device |
| **DEK** (data key) | client, one per file, at upload | never stored unsealed | VK |

The server stores `wrapped_vk_password`, one `wrapped_vk_device` per enrolled device, and one
`wrapped_dek` per file. It can open none of them.

### Primitives

| Step | What is used |
|---|---|
| MK derivation | libsodium `crypto_pwhash`, Argon2id, `OPSLIMIT_MODERATE` / `MEMLIMIT_MODERATE` |
| VK and DEK generation | `randombytes_buf`, 32 bytes |
| Sealing a key | XChaCha20-Poly1305, random 24-byte nonce |
| Sealing a chunk | XChaCha20-Poly1305, nonce = `BLAKE2b(key = DEK, message = chunk index)` |
| Device key pair | X25519 (libsodium) **and** ML-KEM-768 (OpenSSL 3.5+ `EVP_PKEY_encapsulate`) |
| Sealing VK to a device | both shared secrets bound into one key by BLAKE2b over the full transcript |

Login uses `crypto_pwhash_str` at the INTERACTIVE tier and is a different job with different
tuning: it protects one session against an offline crack of a leaked `users.json`. MK derivation is
deliberately heavier, because it stands between a password and every file the account holds. The
parameters are stored per account, so they can be raised later without breaking existing vaults.

ML-KEM-768 here is **not** the TLS code path. `SSL_CTX_set1_groups_list(ctx, "X25519MLKEM768")`
negotiates ML-KEM inside a handshake and exposes no general encapsulate/decapsulate, so the vault
calls `EVP_PKEY_encapsulate`/`EVP_PKEY_decapsulate` directly. Only the OpenSSL 3.5+ dependency is
shared. No liboqs.

## How a file is encrypted

Plaintext is split into chunks of `CHUNK_SIZE - 16` bytes and each chunk is sealed under the file's
DEK. Plaintext plus one 16-byte Poly1305 tag then lands on exactly `CHUNK_SIZE`, which is the point:
every ciphertext chunk but the last is exactly one chunk long, so the `offset = CHUNK_SIZE * index`
arithmetic on both sides is untouched by encryption, and so are chunk validation, the whole-file
hash, and resume.

A deterministic nonce is safe here because it is derived from `(DEK, chunk index)` and every file
has its own DEK — a (key, nonce) pair is therefore never reused across two different plaintexts.

Each chunk is additionally authenticated against its **index and whether it is the last one**.
Without that, a server could reorder chunks, or silently truncate a file, and every individual chunk
would still authenticate perfectly. With it, all four of a wrong key, a modified chunk, a reordered
chunk and a truncated file come out as the same refusal.

**Uploads** encrypt into a scratch file under `data/client_cwd/.vault-tmp/` and then run the
ordinary transfer over that file. The server verifies and stores the bytes it is given exactly as it
always has. **Downloads** write the server's ciphertext to the usual `.part` file and open it once,
after the last chunk — which is what keeps resume working unchanged — into a temporary file that is
renamed into place only when the whole file has decrypted and matched its recorded plaintext hash.

## Why SYNC still works

`compute_sync_plan()` compares hashes. The local side hashes local plaintext; the server cannot
produce a comparable hash, because independent per-file keys mean identical content yields different
ciphertext. So the client records the plaintext hash at upload time, the server stores it in the
manifest, and `SYNC` reports **that** rather than hashing what is on disk.

Every leg of the diff is therefore back in the plaintext domain, `compute_sync_plan()` needs no
changes at all, and move and copy detection keep working — identical content still collapses to a
`MOVE_REMOTE`/`COPY_REMOTE` instead of a re-upload. A server-side `COPY` duplicates the source's
sealed key along with its bytes; that is not nonce reuse, it is the same plaintext under the same
key by construction, which is exactly what makes the cheap copy valid.

## Devices

Any device that can prove the password can join the vault: log in, derive MK, unwrap VK, generate a
local hybrid key pair, seal VK to it, send the public halves. A device is a **convenience, not a
second trust boundary** — the password path already re-proves identity on every login. What it buys
is skipping a second of Argon2id, and giving the server something it can delete to cut off one
machine.

`REVOKE_DEVICE` deletes that row. Note what it does and does not mean: it stops future
server-mediated unwraps, and it does **not** rotate VK, so a copy that device already holds stays
valid. Real revocation would mean rotating VK, re-sealing every DEK and re-sealing to every
remaining device.

## Recovery: there is none

If the password is lost and no enrolled device is reachable, VK — and every file sealed under it —
is **permanently unrecoverable**. The server holds nothing that helps, which is the whole point.
This matches Bitwarden and Proton, and it is a deliberate trade, not an oversight.

A printed recovery key (an alternate sealing of VK, shown once) would soften this and is not built.

## What is not protected

- **Filenames, sizes, modification times and directory structure are not encrypted.** The server
  still sees the shape of the tree. Only contents are sealed.
- **The vault means little below `--rung 3.5`.** See the top of this page.
- **Device private keys sit on disk in the clear**, in `data/client_cwd/.vault/devices.json`, mode
  `0600`. They are worth exactly what that file's permissions are worth. A real deployment would
  hand them to an OS keystore.
- **The password stays in client memory for the session**, so `VAULT_INIT` can derive MK later. It
  is wiped on exit, along with VK.
- **Revoking a device does not rotate VK**, as above.
- **No re-encryption-on-edit optimization**: any content change means a fresh DEK and a full
  re-upload of that file, at the same granularity `SYNC` already uploads at.
- **Zero-byte files still cannot be transferred**, a limitation that predates the vault.
