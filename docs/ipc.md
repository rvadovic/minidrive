# Headless mode (`--ipc`)

MiniDrive's client has two consoles. By default it drives a terminal. Given `--ipc <endpoint>` it
drives a socket instead, speaking length-prefixed JSON frames, and everything else about it is
unchanged: the same state machine, the same wire protocol, the same SYNC/batch/resume engines and
the same vault crypto.

This is the mode the desktop GUI uses. The GUI bundles this binary as a sidecar and drives it over
this channel, rather than reimplementing the sync diff, the batch engine or the key hierarchy in a
second language — see "Desktop GUI — Design" in `CLAUDE.md` for why that decision was made.

It is also why the client builds on Windows and macOS at all. The only unportable part of the
client is the interactive terminal UI; a headless build does not compile it.

```
client [username@]<host>:<port> --ipc /run/user/1000/minidrive.sock
```

## Who owns the endpoint

**The host creates the endpoint and the client connects to it.** The client never listens and never
unlinks anything, so socket lifetime, permissions and cleanup stay with the supervising process,
and a client that dies leaves nothing behind.

If the endpoint cannot be opened, the client prints the reason to stderr and exits with status 1.
It does not fall back to a terminal, and a headless build has no terminal to fall back to.

| Platform | Endpoint |
|---|---|
| Linux, macOS | Path to a Unix domain socket the host has already `bind`/`listen`ed |
| Windows | Named pipe, either `\\.\pipe\name` or a bare `name` (the prefix is added) |

Put the socket somewhere only the user can reach — anyone who can connect to it can drive the
client, and a vaulted session holds the vault key in memory.

## Framing

Every frame, in both directions:

```
+--------------------+---------------------------+
| 4-byte big-endian  | JSON body (UTF-8)         |
| body length        |                           |
+--------------------+---------------------------+
```

This is the same framing the client↔server control channel uses, so a host implements one framing
rather than two. Frames longer than 64 MiB (`transport::MAX_MESSAGE_SIZE`) are refused and the
channel is closed, rather than honoured as an allocation.

## Host → client

One message type. `line` is exactly the command line the interactive CLI would accept, which is
what keeps every command handler unaware of which console it is running under.

```json
{"type": "COMMAND", "line": "UPLOAD report.pdf reports/2026.pdf"}
```

Frames that are not JSON objects, or whose `type` is unknown, are logged and ignored.

**Lines are queued, not dispatched on arrival.** The client takes one only when it asks for input.
This matters: pushing `UPLOAD …` and `EXIT` back to back must not let the `EXIT` abort the transfer,
and it does not — the same property buffered stdin has (Known Bug #1).

## Client → host

### `RESULT` — the outcome of a command

```json
{"type": "RESULT", "ok": true,  "code": 200, "message": "Upload successful"}
{"type": "RESULT", "ok": false, "code": 404, "message": "File does not exist."}
```

`code` is the protocol status code (`shared/include/protocol/codes.hpp`), which is HTTP-shaped.
This is the `OK: …` / `ERROR: <code> …` line the CLI prints, structured.

### `INFO` — free-form output

Listings, vault status, per-conflict notices, `HELP` output. One frame per line.

```json
{"type": "INFO", "text": "  * hot  -  NVMe  (current)"}
```

### `PROMPT` — the client is waiting for input

Emitted when input is armed and nothing is queued, so it is a reliable "idle, waiting for you"
signal as well as a statement of what kind of answer is wanted.

```json
{"type": "PROMPT", "kind": "command",  "text": ""}
{"type": "PROMPT", "kind": "password", "text": "Password for alice: "}
{"type": "PROMPT", "kind": "confirm",  "text": ""}
```

| `kind` | Expected reply |
|---|---|
| `command` | A command line |
| `password` | The account password. Reply with the password alone; it is never echoed back |
| `confirm` | `y` or `n` — the server asked a question (register this user? migrate this tier? revoke this device?) |

### `EVENT` — push notifications

Transfer progress, so a host does not have to poll. Throttled to roughly one per 150 ms, with the
first and last reading of each transfer always sent, so a progress bar starts at 0 and finishes at
100 rather than wherever the throttle landed.

```json
{"type": "EVENT", "event": "transfer_progress", "direction": "upload",
 "path": "/home/alice/report.pdf", "transfer_id": 7,
 "chunks_done": 12, "chunks_total": 40, "bytes_total": 10485760}
```

`direction` is `upload` or `download`. For a vaulted upload, `path` is the ciphertext scratch file
the transfer actually reads from, and `bytes_total` is its size — slightly larger than the
plaintext, by one authentication tag per chunk.

## A session, end to end

```
host                                   client
  |  <-- RESULT  ok, "Secure connection: TLSv1.3, …"
  |  <-- PROMPT  kind=password, text="Password for alice: "
  |  --> COMMAND "hunter2"
  |  <-- RESULT  ok, "Authentication successful."
  |  <-- PROMPT  kind=command
  |  --> COMMAND "UPLOAD report.pdf reports/2026.pdf"
  |  <-- RESULT  ok, "Starting upload to file: reports/2026.pdf"
  |  <-- EVENT   transfer_progress 0/40
  |  <-- EVENT   transfer_progress 18/40
  |  <-- EVENT   transfer_progress 40/40
  |  <-- RESULT  ok, "Upload successful"
  |  <-- PROMPT  kind=command
  |  --> COMMAND "EXIT"
```

## Shutting down

Either send `EXIT` as a command, or close the socket. Both end the session cleanly: an in-flight
transfer is aborted with its state saved, so it remains resumable, and the client exits 0.

## What headless mode does not change

`--ipc` composes with every other flag. In particular the TLS posture is unchanged — a GUI session
uses `--rung 3.5` with `--ca-file`/`--pin` exactly as the CLI does (`docs/tls.md`), and vault
commands work identically (`docs/vault.md`), because the key hierarchy runs in this process and
nowhere else.

The interactive CLI remains Linux-only and is unaffected by any of this.
