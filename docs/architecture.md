# MiniDrive Architecture

## High-Level Components

- **Client (`client/`)**
  - Command-line interface with an interactive shell and line editor (history, arrow-key
    editing) built on raw POSIX terminal handling.
  - State machine (`ClientState`) mirroring the server's closely, by design — upload is "server
    downloads from the client's perspective" and vice versa, so the two are near-symmetric.
  - Transfer manager driving chunked binary streaming over the same TCP connection as the control
    channel.
  - A generic batch/queue engine that drives `SYNC`, `UPLOAD_DIR`/`DOWNLOAD_DIR`, and batch
    `DELETE`/`MOVE`/`COPY` through the same one-item-at-a-time execution loop, so a 500-file sync
    and a single `UPLOAD` share almost all of their code.
  - Signal-aware shutdown, saving resume state on interruption.
  - Local file integrity validation (post-transfer hashing) via libsodium.
  - Structured logging (spdlog) to a file only — the client's stdout is a stable `OK`/`ERROR`
    protocol other tools can script against, so nothing else is allowed to write to it.
- **Server (`server/`)**
  - Listener (`Server`) accepting TCP connections via Asio, on a thread pool sized to
    `hardware_concurrency()`.
  - `Session` — one per connection, a large state machine (`SessionState`) handling every command:
    auth/registration, file/folder operations, `SYNC`'s listing, storage-tier commands, and the
    resume-negotiation handshake.
  - `Storage` — resolves each user's effective root (accounting for storage tiering), holds a
    per-user busy-flag "lock" (not a queue — a second concurrent operation for the same user gets
    an immediate `503`, never blocks), and lazily constructs each user's `PartialMetadata`
    (resumable-transfer tracker).
  - `Database` — a single mutex-guarded, file-based JSON store of users (username, password hash,
    assigned storage tier).
  - Structured logging (spdlog) to a rotating file, optionally mirrored to stdout.
- **Shared (`shared/`)**
  - Wire protocol types and JSON (de)serialization (`protocol::Request`/`Response`/`ChunkInfo`/
    `FileEntry`/`TierInfo`).
  - `fsutils` — hashing (libsodium BLAKE2b via `crypto_generichash`), chunking, and path-safety
    helpers, consistently using non-throwing `(path, ec)` `std::filesystem` overloads so a
    filesystem error can never escape into an Asio handler and crash the process.
  - `PartialMetadata` — a file-based JSON database of in-flight resumable transfers, one per user.
  - `minidrive::log` — the spdlog setup shared by both binaries (`init(name, file, level,
    also_console)`).
  - `minidrive::version()`/`resolved_version()` — build-time version reporting (see "Versioning").

## Protocol Shape

Two framings share one TCP connection, switched by each side's own state:

- **Control channel**: a 4-byte big-endian length prefix followed by a JSON body
  (`protocol::Request`/`Response`).
- **Binary channel** (active only during a transfer): a small fixed `ChunkHeader` (transfer id,
  chunk index, size, flags — packed, network byte order) followed by the raw chunk bytes.

See [protocol.md](protocol.md) for the full message shapes and [flows.md](flows.md) for the
control/data phase sequencing of uploads, downloads, resume, and sync.

## Synchronization Engine

`SYNC` is a three-way merge, not a naive "newer file wins": each side of a local↔remote pair keeps
a small manifest recording what was last successfully synced (the *baseline*). Every `SYNC` run
classifies every path by comparing local vs. remote vs. that baseline, which is what lets it tell
"changed locally" apart from "changed on both sides to different content" (a real conflict) — a
plain mtime comparison can't distinguish those and risks silently discarding an edit. Conflicts
never lose data: the local file is kept and uploaded as-is, and the remote's conflicting version is
additionally downloaded as a `... (conflict copy from server, <timestamp>)` file.

`UPLOAD_DIR`/`DOWNLOAD_DIR` and batch `DELETE`/`MOVE`/`COPY` reuse the exact same queue-draining
engine as `SYNC` — a directory upload is just "queue everything unconditionally" where `SYNC` is
"queue only what the three-way diff says differs." No new wire-protocol primitive was needed for
any of this: the server's single-item handlers (`upload`/`download`/`delete`/`mkdir`/`move`/`copy`)
are unmodified, driven once per queued item by the client.

## Storage Tiering

The server can span several physical media, each declared by name on the command line:

```sh
./server --port 9000 --root /srv/minidrive \
  --tier hot=/mnt/ssd     --tier-desc hot="NVMe SSD, fast" \
  --tier archive=/mnt/hdd --tier-desc archive="7200rpm HDD, bulk" \
  --default-tier hot
```

- `--root` is the **control root**: it holds `users.json` and the shared `public/` directory.
- Each `--tier` path holds `private/<user>/{files,.partial}` for the users placed on it.
- With no `--tier` flag, the server synthesizes a single implicit tier named `hot` pointing at
  `--root`, which reproduces the original single-root layout exactly — tiering is fully opt-in.
- A user's tier is recorded per-user in `users.json` as `storage_class`. New registrations get
  `--default-tier` (default `hot`); an entry without the field reads as the default tier.
- Startup refuses duplicate/malformed tier names, missing tier paths, overlapping tier paths, and
  an undeclared default tier. It warns (doesn't refuse) when two tiers share a filesystem, since
  that's a legitimate test setup even though it defeats the point of tiering in production.

Clients discover the configured media with `TIERS` and move themselves with `SET_TIER <name>`. The
server sends only tier names and descriptions, never filesystem paths, and rejects any tier name it
wasn't configured with. `SET_TIER` asks for confirmation, then copies the user's tree to the target
medium, verifies every file's hash against the source, and only then removes the original — a
failure at any point before that final removal leaves the source untouched.

## Logging

Both binaries link `spdlog` and go through one shared init point, `minidrive::log::init(name,
file, level, also_console)` (`shared/include/minidrive/logging.hpp`):

- **Server**: `--log-file <path>` (rotating, 5 MiB × 3 backups) plus stdout, `--log-level` (default
  `info`).
- **Client**: `--log <path>`, **file sink only, never console** — every log call in the client is
  additional to, and completely separate from, the `OK`/`ERROR` lines it prints for the user (and
  that other tools parse). Nothing sensitive is logged: passwords are redacted before an `AUTH`
  request is written to the log.

## Versioning

`minidrive::version()` is the plain project version (`project(MiniDrive VERSION x.y.z)` in the
root `CMakeLists.txt`, single-sourced into `shared/include/minidrive/version.hpp.in` via
`configure_file`). `minidrive::resolved_version()` prefers `git describe --tags --always --dirty`,
captured at configure time, so a binary reports exactly what commit (and how many commits past the
last tag, and whether the tree was dirty) it was built from — falling back to the plain version
string when the source tree isn't a git checkout (e.g. a downloaded source archive).

## Releases

`cmake/Packaging.cmake` configures CPack to produce a `.tar.gz` and a Debian `.deb` from the
`install()`-staged `server`/`client` binaries only — no source, no tests, no dev-container files.
Both binaries are built with `-static-libgcc -static-libstdc++` (`MINIDRIVE_STATIC_RUNTIME`,
default on for non-MSVC builds); combined with the header-only dependencies (Asio, nlohmann/json,
spdlog) and a from-source libsodium build, a release binary's only shared-library dependency is
glibc. `.github/workflows/release.yml` builds, runs the gating test suites, packages, and publishes
a GitHub Release whenever a `vX.Y.Z` tag is pushed; `.github/workflows/ci.yml` does the same build
and test on every push/PR to `main`.

## Roadmap

MiniDrive has no transport encryption yet — the control channel (including the password sent
during login) is plaintext TCP. Closing that gap with TLS, then mutual TLS, then a post-quantum
hybrid key exchange, is the next major body of work, alongside a REST API and cross-platform
desktop GUI so the system is usable without the (currently Linux-only) CLI client. None of that
is implemented yet; this section will move as it lands.

## Directory Layout

```
.
├── cmake                             # Dependency fetching and release packaging
│   ├── Dependencies.cmake
│   └── Packaging.cmake
├── .github/workflows                 # CI and release automation
│   ├── ci.yml
│   └── release.yml
├── CMakeLists.txt                    # Root build orchestrator (version, git describe, install)
├── client
│   ├── CMakeLists.txt
│   ├── include
│   │   ├── client.hpp
│   │   ├── sync_diff.hpp             # Pure three-way merge diff algorithm
│   │   ├── sync_manifest.hpp         # Per-pair sync baseline (file-based JSON DB)
│   │   ├── terminalNoEcho.hpp        # No-echo password entry
│   │   └── terminalRaw.hpp           # Raw-mode line editor (history, arrow keys)
│   └── src                           # client.cpp, main.cpp, sync_diff.cpp, sync_manifest.cpp, ...
├── server
│   ├── CMakeLists.txt
│   ├── include
│   │   ├── database.hpp              # User database manager
│   │   ├── password.hpp              # Password hashing helper
│   │   ├── server.hpp
│   │   ├── session.hpp               # Per-connection state machine
│   │   ├── storage.hpp               # Filesystem/tiering manager
│   │   └── tier_config.hpp           # StorageTier/StorageConfig
│   └── src                           # database.cpp, main.cpp, password.cpp, server.cpp, session.cpp, storage.cpp
├── shared
│   ├── CMakeLists.txt
│   ├── include
│   │   ├── filesystem                # fsutils: hashing, chunking, path safety
│   │   ├── minidrive                 # logging.hpp, version.hpp.in
│   │   └── protocol                  # message/commands/statuses/codes/flags
│   └── src                           # mirrors include/, plus version.cpp
├── data                              # git-ignored scratch server/client roots for local dev
├── tests
│   ├── CMakeLists.txt
│   └── integration                   # black-box test suites (see README.md)
├── docs
│   ├── architecture.md               # this file
│   ├── flows.md                      # control/data phase sequencing
│   ├── protocol.md                   # wire format reference
│   └── requirements.md               # feature specification
└── README.md
```