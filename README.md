# MiniDrive

MiniDrive is a client/server file-synchronization system written in modern C++20 — a self-hosted,
NAS-style alternative to Dropbox/OneDrive that you run yourself. A single `server` binary owns a
directory tree (optionally spread across several storage tiers) and any number of `client`
sessions connect to it over TCP to browse, transfer, and synchronize files, with or without
authentication.

It started as a from-scratch exploration of building a real client/server protocol on top of
[Asio](https://think-async.com/Asio/) and has grown into an independent project with its own
roadmap — see [docs/requirements.md](docs/requirements.md) for the full feature specification and
[docs/architecture.md](docs/architecture.md) for how it's built.

## Features

- **Public and private modes** — connect anonymously to a shared public directory, or authenticate
  (register on first use) to get a private, isolated directory tree. Passwords are salted and
  hashed with libsodium's Argon2id, never stored or logged in plaintext.
- **Full file/folder command set** — `LIST`, `UPLOAD`, `DOWNLOAD`, `DELETE`, `CD`, `MKDIR`,
  `RMDIR`, `MOVE`, `COPY`, plus batch variants of `DELETE`/`MOVE`/`COPY` that take multiple paths
  at once.
- **Whole-directory transfer** — `UPLOAD_DIR`/`DOWNLOAD_DIR` walk a local or remote tree and queue
  every file, creating directories as needed.
- **Two-way `SYNC`** — a three-way merge (local vs. remote vs. last-known-synced baseline) that
  uploads local changes, pulls remote changes, propagates deletes in either direction, detects
  renames/moves and same-content copies to avoid redundant transfers, and never silently discards
  a conflicting edit — it keeps both sides and saves the remote version as a
  `... (conflict copy from server, <timestamp>)` file.
- **Resumable transfers** — an interrupted upload or download (server crash, dropped connection)
  is picked back up on reconnect from the last acknowledged chunk, not from scratch. Private-mode
  only, since there's nothing durable to resume in the shared public directory.
- **Chunked binary transfers** — files move in 256 KiB chunks over the same TCP connection as the
  control channel, each chunk hash-verified on arrival, with a full-file hash check at the end.
- **Multiple concurrent sessions** — many clients (even the same user, from different machines)
  can be connected at once; per-user file operations are serialized so nothing corrupts, but
  sessions are never rejected outright.
- **Per-user storage tiering** — the server can be handed several physical storage roots
  (`--tier hot=/mnt/ssd --tier archive=/mnt/hdd ...`) and place each user on one of them. Clients
  list the available media with `TIERS` and relocate their own data with `SET_TIER`, which
  copies, verifies, and only then removes the original.
- **Structured logging** — both `server` and `client` can log to a rotating file via spdlog
  (`--log-file`/`--log` respectively, plus `--log-level`). The client's log is file-only by
  design; its stdout is a stable, scriptable `OK`/`ERROR` protocol (see
  [docs/protocol.md](docs/protocol.md)), never mixed with log output.
- **Version reporting** — `minidrive::resolved_version()` reflects the exact git commit/tag a
  binary was built from (`git describe`), so a bug report's version string is unambiguous.

## Getting started

### Option 1: download a release

Prebuilt, statically-linked Linux binaries (`server` + `client`) are published on the
[Releases page](https://github.com/rvadovic/minidrive/releases) as a `.tar.gz` (any distro) and a
`.deb` (Debian/Ubuntu). No runtime dependencies beyond glibc.

```sh
tar xzf minidrive-*.tar.gz
./usr/bin/server --port 9000 --root ./data
./usr/bin/client 127.0.0.1:9000
```

### Option 2: build from source

Requires CMake 3.22+ and a C++20 compiler. Dependencies (Asio, nlohmann/json, spdlog, libsodium)
are fetched automatically via `FetchContent` on first configure.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

Binaries land at `build/server/server` and `build/client/client`. MiniDrive currently targets
Linux (POSIX terminal handling, `asio::posix::stream_descriptor`); a cross-platform GUI is on the
roadmap instead of porting the CLI (see [docs/architecture.md](docs/architecture.md)).

## Running

```sh
# Server: single flat root
./build/server/server --port 9000 --root ./data/server_root

# Server: multiple storage tiers
./build/server/server --port 9000 --root ./data/control \
  --tier hot=./data/hot --tier-desc hot="fast SSD" \
  --tier archive=./data/archive --tier-desc archive="bulk HDD" \
  --default-tier hot

# Server logging
./build/server/server --port 9000 --root ./data/server_root \
  --log-file server.log --log-level info

# Client, public mode
./build/client/client 127.0.0.1:9000

# Client, private mode (prompts for a password; registers on first use)
./build/client/client alice@127.0.0.1:9000

# Client logging (file only - never mixed with the OK/ERROR stdout protocol)
./build/client/client alice@127.0.0.1:9000 --log client.log --log-level debug
```

Once connected, type `HELP` at the `>` prompt for the full command list. See
[docs/requirements.md](docs/requirements.md) for what each command does and
[docs/protocol.md](docs/protocol.md) for the wire format behind it.

## Environment Variables

The dev container sets these via `containerEnv` (see `.devcontainer/devcontainer.json`). Useful
for local development; not read by the release binaries themselves.

| Variable | Purpose | Default |
|----------|---------|---------|
| `MINIDRIVE_HOST` | Host/IP the client connects to; server binds 0.0.0.0 | `127.0.0.1` |
| `MINIDRIVE_PORT` | TCP port for server listen + client connect | `9000` |
| `MINIDRIVE_USERNAME` | Username for VS Code launch configs | (empty) |

## VS Code Tasks

Defined in `.vscode/tasks.json`:

- `project-configure` – CMake configure (exports compile commands)
- `project-build` – Build targets
- `run-server` – Run server (w/o attached debugger) with port/root
- `run-client` – Run client (w/o attached debugger) connecting host:port
- `terminate-server` – SIGTERM active server process

Use the Command Palette > Run Task to invoke any of them.

## Debugging

Launch configurations (`.vscode/launch.json`):

- `Debug Server` – Builds then starts server under gdb
- `Debug Client` – Starts the client

To debug both, run two separate debug sessions and switch between them via the Debug Console
dropdown.

## Testing

`tests/integration/*.py` is a black-box suite that spawns real `server`/`client` binaries and
drives the client over stdin/stdout — the ground truth for "does this actually work."

```sh
cmake --build build --target integration_smoke
ctest --test-dir build --output-on-failure   # dependency-linkage smoke test

python3 tests/integration/run_all_tests.py                 # full suite
python3 tests/integration/run_all_tests.py --suite auth     # one suite
python3 tests/integration/run_all_tests.py --list           # list suite IDs
```

CI (`.github/workflows/ci.yml`) runs the build and the suites that are expected to pass outright
on every push; a small number of suites have known, documented test-harness gaps (not
server/client bugs) and run separately as non-blocking — see the comments in that workflow file.

## Releases

Tagging a commit `vX.Y.Z` and pushing the tag triggers `.github/workflows/release.yml`, which
builds, runs the gating test suites, packages the binaries with CPack (`.tar.gz` + `.deb`), and
publishes a GitHub Release with those artifacts attached.

## Repository Layout

- `client/`, `server/`, `shared/` – application targets (`shared/` holds the wire protocol,
  filesystem helpers, logging, and version reporting used by both)
- `cmake/` – dependency fetching (`Dependencies.cmake`) and release packaging (`Packaging.cmake`)
- `.github/workflows/` – CI and release automation
- `docs/` – architecture, protocol, flow, and feature-specification documentation
- `data/` – git-ignored scratch directory for local server/client roots during development
- `tests/` – black-box integration test suite

See [docs/architecture.md](docs/architecture.md) for how the pieces fit together.