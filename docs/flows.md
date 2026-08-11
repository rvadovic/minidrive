# Protocol Flows

Sequencing of control-channel and binary-channel messages for each multi-step operation. See
[protocol.md](protocol.md) for the exact message shapes referenced here.

## Connection & Authentication

1. Client connects, then immediately sends `LOGIN` (username empty for public mode).
2. **Public mode** (empty username, or server replies without needing a password): server responds
   `OK` and the session is immediately ready for commands.
3. **Private mode, known user**: server responds `AUTH` ("Please provide your password."). Client
   prompts (no local echo) and sends `AUTH` with the password. Server validates against the stored
   Argon2id hash and responds `OK` (ready) or `AUTH`/`UNAUTHORIZED` (retry).
4. **Private mode, unknown user**: server responds `NEED_INPUT` ("Register? (y/n):"). Client sends
   `NEED_INPUT` with `y`/`n`. On `y`, server responds `AUTH` asking for a new password; client sends
   it via `AUTH`; server creates the account and responds `OK`.
5. If the newly-authenticated user has incomplete transfers on record, the server sends exactly one
   `OK`/`RESUME` response, never both back-to-back — see "Resume Negotiation" below. This avoids a
   race where a second immediate response collides with input the client already has buffered.

## Upload

### Control Phase
1. Client hashes the local file (whole-file + per-chunk), sends `UPLOAD` with that metadata and
   the `ChunkInfo` list.
2. Server validates the request (path safety, destination doesn't already exist, user not already
   mid-operation) and responds `OK`. Both sides switch `state_` to `UPLOADING`, which also switches
   the socket's framing from JSON to the binary chunk protocol.

### Data Phase
- Client sends binary chunks (`SEND` flag, or `LAST` for the final one) with a `ChunkHeader`.
- Server verifies each chunk's hash/size against the negotiated plan and acknowledges with the
  same header shape, `flags = OK` (or `CHUNK_MISMATCH` on a mismatch, which aborts the transfer).
- On the last chunk, the server also verifies the whole-file hash and acknowledges with `DONE`.
- Either side can send `flags = EXIT` to abort mid-transfer (e.g. `Ctrl+C`); the receiving side
  cleans up its partial file/metadata.

## Download

### Control Phase
1. Client sends `DOWNLOAD` naming the remote file.
2. Server validates the request, responds `OK` with the file's metadata and `ChunkInfo` list, and
   switches to `DOWNLOADING`.
3. Client prepares a local `.part` file and also switches to `DOWNLOADING`.

### Data Phase
- Server sends binary chunks (`SEND`/`LAST`) the same way an uploading client would.
- Client verifies each chunk and acknowledges (`OK`/`CHUNK_MISMATCH`), and on the last chunk
  verifies the whole-file hash before acknowledging `DONE` and renaming the `.part` file into
  place.

## Resume Negotiation

Private-mode only; reuses the ordinary `NEED_INPUT` mechanics rather than a dedicated protocol
command. Triggered right after a successful login/registration, in place of the plain `OK`:

1. Server checks the newly-authenticated user's `PartialMetadata` for incomplete transfers. If
   there are any, it sends one `RESUME` response per queued entry, in sequence, each shaped like:
   `message = "Incomplete upload/downloads detected, resume? (y/n):\nUPLOAD <path>"`.
2. Client shows the prompt and sends `NEED_INPUT` (`y`/`n`).
3. **On `y`**: server sends a second `RESUME` response — the *kickoff* — where `file_hash` carries
   the transfer id (decimal string) and `message` is just `"UPLOAD <path>"`/`"DOWNLOAD <path>"`.
   - For a resumed **upload**, the client looks up its own local record for that transfer id (it's
     the only place the source file path is known — the server only knows the destination), seeds
     `transfer_` from it without re-negotiating chunks, and the connection switches straight to the
     binary channel with the client sending — starting from the first chunk the server doesn't
     already have.
   - For a resumed **download**, the same happens in the other direction; the server is already
     driving, so it starts sending from the first chunk index the client hasn't acknowledged.
4. **On `n`**: the server deletes both the `.part` file and the metadata entry, and moves on to the
   next queued transfer (if any) or a plain `OK`.
5. Once every queued transfer has been offered, the session is `READY` for ordinary commands.

An interrupted transfer is recorded as resumable on both sides independently — the server via
`PartialMetadata`, the client via its own local partial-metadata store — so either a server crash
(`SIGTERM`) or a dropped connection leaves enough state on both ends to resume correctly, without
re-transferring bytes already acknowledged.

## Sync

`SYNC` is stateless on the wire — one request/response pair returns a listing, and every actual
change is driven by ordinary single-item commands the server already implements:

1. Client sends `SYNC <remote_dir>` (as `first_argument`). Server validates the directory exists,
   recursively scans it, and responds with a `files` listing (path, size, hash, mtime,
   is-directory) for everything under it.
2. Client recursively scans the corresponding local directory the same way, loads its persisted
   sync baseline (what local/remote looked like after the last successful sync of this pair), and
   computes a three-way diff (local vs. remote vs. baseline) — see
   [requirements.md](requirements.md#synchronization) for the classification rules.
3. The diff becomes an ordered queue of ops: remote `MKDIR`s first (parent before child), then
   `MOVE`s, then `DELETE`s/`RMDIR`s, then `UPLOAD`/`DOWNLOAD`/copy-as-`COPY` operations, with any
   conflict-copy downloads pulled to the very front so a same-path replace's delete can't destroy
   the conflict copy's source bytes first.
4. The client drains that queue one item at a time — each op is an ordinary `UPLOAD`/`DOWNLOAD`/
   `DELETE`/`MOVE`/`COPY`/`MKDIR` request, using the exact same control/data phases described
   above. The next op is only dispatched once the previous one's response (or, for a transfer, its
   terminal chunk) has been fully handled — so a `SYNC` never holds more than one server-side
   operation in flight, which matters because the server's per-user lock is not reentrant.
5. After each op succeeds, the client updates and persists its local baseline immediately — so an
   interrupted `SYNC` (network drop, `Ctrl+C`) simply leaves the baseline reflecting whatever
   actually completed, and re-running `SYNC` recomputes exactly the correct remaining diff. No
   separate resume mechanism is needed for `SYNC` itself; any in-flight file transfer inside it
   still gets full chunk-level resumability from the mechanism above.
6. One item failing doesn't stop the rest — failures are counted and listed in the final summary.

`UPLOAD_DIR`/`DOWNLOAD_DIR` and batch `DELETE`/`MOVE`/`COPY` reuse this same queue-draining engine;
the only difference is how the queue is built (unconditionally, from a directory walk, instead of
from a three-way diff).

## Storage Tier Change

1. Client sends `SET_TIER <name>`.
2. Server checks: session is ready, the name isn't empty, the calling user isn't `public`, the
   named tier is configured, the user isn't already on it, and the user has no pending resumable
   transfers (their entries store absolute destination paths, which a migration would invalidate).
   Passing all of that, it acquires the user's lock and responds `NEED_INPUT` asking for
   confirmation — the lock is held across this round-trip so a concurrent upload can't start
   mid-migration, but is released unconditionally if the client disconnects without answering.
3. Client sends `NEED_INPUT` (`y`/`n`).
4. On `y`, the server copies the user's directory tree to the target tier, verifies every file's
   hash against the source, removes the original only once verified, updates the user's recorded
   tier, and responds `OK`. On `n`, it releases the lock and responds `OK` with nothing changed.