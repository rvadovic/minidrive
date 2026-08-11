# MiniDrive Feature Specification

This document describes what MiniDrive's `server` and `client` actually do today — a living
specification, not a fixed one-time brief. When behavior changes, this file changes with it. For
how it's implemented, see [architecture.md](architecture.md); for the exact wire format, see
[protocol.md](protocol.md).

## Overview

MiniDrive is a client/server file-synchronization system, similar in spirit to Dropbox/OneDrive
but self-hosted: one `server` process owns a directory tree, and any number of `client` sessions
connect over TCP to browse, transfer, and synchronize files against it.

- **Platform**: `server` and `client` currently target Linux (POSIX terminal handling, standalone
  Asio). A cross-platform GUI talking to a future REST API is planned instead of porting the CLI —
  see [architecture.md](architecture.md).
- **Build system**: CMake 3.22+; dependencies (Asio, nlohmann/json, spdlog, libsodium) are fetched
  automatically.

## Server

```
server --port <PORT> --root <ROOT_PATH>
       [--tier <name>=<path>]... [--tier-desc <name>=<text>]... [--default-tier <name>]
       [--log-file <path>] [--log-level <level>]
```

| Aspect | Behavior |
| :--- | :--- |
| **Binding** | Binds to `0.0.0.0` on the given port, accepts multiple clients concurrently (one thread pool sized to `hardware_concurrency()`, async I/O via Asio). |
| **Public mode** (default) | No authentication. All unauthenticated clients share one **public** directory. |
| **Private mode** | Client supplies a username; the server authenticates or registers it and assigns a private directory that only that user's sessions can see. |
| **Root directory** | `--root` is the *control root*: it holds `users.json` and the shared `public/` directory. With no `--tier` flags, it also holds every user's private directory (`<root>/private/<user>/...`) — the classic single-root layout. |
| **Storage tiering** | One or more `--tier <name>=<path>` flags put each user's private directory on a named, separately-located medium instead. See "Storage Tiering" below. |
| **Persistence** | User credentials (`users.json`) and the file tree persist across restarts. In-flight transfers persist too (see "Resuming Transfers"). |
| **Graceful handling** | Invalid commands, missing files, and permission errors return a structured `ERROR: <code>` response, never a crash. |
| **Shutdown** | `SIGTERM`/`SIGINT` closes all active sessions cleanly. |
| **Logging** | `--log-file <path>` writes structured, leveled logs (spdlog, 5 MiB rotating, 3 backups) to a file; `--log-level` (default `info`) controls verbosity. Also mirrored to stdout. |

## Client

```
client [username@]<server_ip>:<port> [--log <log_file>] [--log-level <level>]
```

| Aspect | Behavior |
| :--- | :--- |
| **Connection** | Connects to the given host/port. |
| **Public mode** | No username given. Prints `[warning] operating in public mode - files are visible to everyone`. |
| **Private mode** | Username given: the client prompts for a password (no local echo). If the user exists, it authenticates; on success, prints `OK: Authentication successful.`. |
| **Registration** | If the username doesn't exist on the server, the client prompts `User <username> not found. Register? (y/n):`; on `y`, prompts for a password and registers. |
| **Prompt** | Once connected, an interactive `>` prompt accepts commands, with line editing (history, arrow keys) when running in a real terminal. |
| **Logging** | `--log <log_file>` writes structured logs (client requests, server responses, connection events) to that file only — **never** to stdout/stderr, since stdout is the scriptable protocol contract below. `--log-level` (default `info`) controls verbosity. |
| **Ctrl+C** | `SIGINT` closes the connection gracefully; if a transfer or sync is interrupted this way, its state is saved for resumption. |

## Interactive Commands

Paths are relative to the current directory on the server unless they start with `/` (absolute
within the user's root). Paths never escape that root — directory traversal (`../`) is rejected.
Commands and paths are case-sensitive. Every remote command produces exactly one response:

```
OK: <message>
<optional additional lines>
```
or
```
ERROR: <code> <message>
<optional additional lines>
```

### Local-only

| Command | Description |
| :--- | :--- |
| `HELP` | Lists available commands. |
| `EXIT` | Closes the connection and exits. |

### File & folder commands

| Command | Description |
| :--- | :--- |
| `LIST [path]` | Lists files and folders at `path` (current directory if omitted). |
| `UPLOAD <local_path> [remote_path]` | Uploads one local file. Fails if `remote_path` already exists — delete or rename first. |
| `DOWNLOAD <remote_path> [local_path]` | Downloads one remote file. Fails if the local file already exists. |
| `DELETE <path> [path...]` | Deletes one or more remote files. Each path is a fully independent operation — one failure doesn't stop the rest. |
| `CD <path>` | Changes the current remote directory. |
| `MKDIR <path>` | Creates a remote folder. Fails if it already exists. |
| `RMDIR <path>` | Recursively removes a remote folder. |
| `MOVE <src...> <dst>` / `COPY <src...> <dst>` | Moves/copies one path to `<dst>`, or several paths into `<dst>/` when given more than one source (or a trailing `/`). Fails per-item if a destination already exists. |
| `UPLOAD_DIR <local_dir> [remote_dir]` | Uploads a whole local directory tree (creating remote folders as needed), unconditionally — every file, no diffing. |
| `DOWNLOAD_DIR <remote_dir> [local_dir]` | Downloads a whole remote directory tree the same way. |

### Synchronization

| Command | Description |
| :--- | :--- |
| `SYNC <local_dir> <remote_dir>` | Two-way sync between a local directory and a remote one that must already exist (create it first with `MKDIR`). |

`SYNC` keeps a small local manifest of what was uploaded/downloaded last time (a "baseline") and
does a three-way comparison — local vs. remote vs. that baseline — for every file:

- Changed only locally → **upload**.
- Changed only remotely → **download**.
- Deleted on one side, unchanged on the other → **delete** it on the other side too.
- Changed on both sides to the *same* content → nothing to do, just record it.
- Changed on both sides to *different* content, or deleted on one side while edited on the
  other → **conflict**: nothing is silently lost. The local file is kept and uploaded as-is; the
  server's version is additionally downloaded as
  `<name> (conflict copy from server, <YYYY-MM-DD HHMMSS>).<ext>`.
- A file that moved (same content, different path) is detected as a rename, not a delete+upload.
  A file with the same content freshly appearing under a new path — when that content is already
  present or arriving elsewhere on the server — is uploaded once and copied server-side instead of
  being sent twice.

One item failing doesn't stop the rest of the sync. A summary line reports what happened:
`OK: Sync complete. Uploaded: 3, Downloaded: 1, Deleted: 2, Moved: 1, Skipped: 5, Conflicts: 1.`

### Storage tiering

| Command | Description |
| :--- | :--- |
| `TIERS` | Lists the storage media the server was configured with (name, description, which one you're currently on). Only meaningful in private mode. |
| `SET_TIER <name>` | Moves your data to another configured medium. Asks for confirmation, then copies your files, verifies every hash matches, and only removes the original once verified. |

## File Handling

- Files transfer in **binary mode**, chunked at **256 KiB** per chunk, so upload/download memory
  use doesn't scale with file size.
- Files up to several GiB are supported; a directory scan (used by `SYNC`/`UPLOAD_DIR`/
  `DOWNLOAD_DIR`) currently refuses any single file over 4 GiB.
- Every chunk is hash-verified on arrival; the whole file is hash-verified again once complete.
- I/O errors (file not found, permission denied, disk full) return a structured error rather than
  crashing the session.

### Resuming Transfers

- Resume is a **private-mode-only** feature — the public directory has no per-connection identity
  to resume against.
- Covers two interruption scenarios: the server received `SIGTERM` mid-transfer, or the connection
  was lost. (A hard client crash, e.g. `SIGKILL`, has no chance to persist local resume state and
  is not resumable from that side.)
- On reconnect, if the server has one or more incomplete transfers recorded for that user, it asks
  before doing anything else:

  ```
  OK: Authentication successful.
  Incomplete upload/downloads detected, resume? (y/n):
  > y
  UPLOAD <file1>
  ```

- Answering `y` resumes from the last acknowledged chunk — only the missing bytes are
  re-transferred. Answering `n` discards the incomplete transfer.
- Multiple incomplete transfers are offered one at a time, in sequence.
- Both sides verify the final file hash once a resumed transfer completes, exactly as with a fresh
  transfer.

### Multiple Sessions

- Any number of clients may connect at once, including several sessions under the same username
  (or several public-mode clients simultaneously) — none are rejected outright.
- Per-user file operations are serialized: a second concurrent write-type request for a user
  already mid-operation gets an immediate `503 Server is busy` rather than corrupting anything or
  blocking indefinitely.
- Concurrent uploads of the same file by two sessions may result in one overwriting the other, but
  never in a corrupted file or a crashed server.

## Communication Protocol

See [protocol.md](protocol.md) for the full wire format with examples. Summary:

| Aspect | Detail |
| :--- | :--- |
| **Control channel** | JSON messages (`nlohmann::json`) over a raw TCP socket, each prefixed with a 4-byte big-endian length. |
| **Binary channel** | File chunks stream over the *same* connection with a small fixed binary header (transfer id, chunk index, size, flags) — no re-encoding to JSON/base64. |
| **Client message** | At minimum a command name (`cmd`) and its arguments. |
| **Server response** | A `status`, a numeric `code`, and a human-readable `message`, plus command-specific fields (file listings, chunk lists, tier lists, ...). |

### Status codes

Response codes are HTTP-shaped for familiarity, defined in `shared/include/protocol/codes.hpp`:

| Code | Meaning |
| :--- | :--- |
| 200 | OK |
| 202 | Accepted (e.g. transfer initiated) |
| 400 | Bad request (missing/invalid argument) |
| 401 | Unauthorized (bad password) |
| 403 | Forbidden (e.g. an operation not allowed in public mode) |
| 404 | Not found |
| 409 | Conflict |
| 412 | Precondition failed (e.g. destination already exists) |
| 500 | Internal server error |
| 503 | Service unavailable (server busy for this user) |

## Authentication and Passwords

| Aspect | Detail |
| :--- | :--- |
| **Authentication** | Optional; public mode is the default. |
| **Credentials** | Provided once, checked/created on the server. |
| **Security** | Passwords are never stored in plaintext. |
| **Hashing** | libsodium `crypto_pwhash_str` (Argon2id), which embeds a per-user salt in the stored hash. |
| **Storage** | A flat, file-based JSON database (`users.json`) — username, password hash, and assigned storage tier. |

Passwords do currently cross the network in plaintext (there is no transport encryption yet) —
see [architecture.md](architecture.md)'s roadmap notes for where that's headed.

## Technical Stack

| Aspect | Detail |
| :--- | :--- |
| **Language** | C++20 |
| **Networking** | Standalone Asio (async TCP) |
| **Serialization** | nlohmann/json |
| **Crypto** | libsodium (password hashing, content hashing) |
| **Logging** | spdlog |
| **Filesystem** | `std::filesystem` throughout, with non-throwing `(path, ec)` overloads on every path-safety-relevant call |
| **Concurrency** | Asio's async model plus a `std::thread` pool on the server |
| **Security** | Every file-touching handler resolves and validates the path against the caller's root before touching disk; traversal outside that root is rejected. |