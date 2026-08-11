# MiniDrive Protocol

The exact wire format spoken between `client` and `server`. For when each message is sent relative
to the others, see [flows.md](flows.md); for what each command *does*, see
[requirements.md](requirements.md). Struct definitions live in `shared/include/protocol/`.

## Framing

One TCP connection carries two framings, and each side tracks its own state (`SessionState` /
`ClientState`) to know which one is currently active:

- **Control channel** — a 4-byte unsigned length prefix in network byte order, followed by that
  many bytes of UTF-8 JSON.
- **Binary channel** — active only while a transfer (`UPLOAD`/`DOWNLOAD`) is in progress: a fixed,
  packed `ChunkHeader` followed immediately by that many raw chunk bytes. No base64, no JSON
  wrapping — the control channel negotiates the transfer, then the connection switches framing.

```cpp
#pragma pack(push, 1)
struct ChunkHeader {
    uint32_t transfer_id; // network byte order
    uint32_t index;       // chunk index within the file, network byte order
    uint32_t size;        // bytes of chunk data following this header, network byte order
    uint8_t  flags;       // see "Chunk flags" below
};
#pragma pack(pop)
```

## Control Channel Messages

### Request (client → server)

```jsonc
{
  "cmd": "UPLOAD",
  "first_argument": "notes.txt",
  "second_argument": "docs/notes.txt",
  "size": 262144,
  "file_hash": "a7d9bd1e...",     // hex-encoded BLAKE2b (crypto_generichash), whole-file
  "chunks": [                      // present only when non-empty
    { "index": 0, "size": 262144, "chunk_hash": "f23cb2..." }
  ]
}
```

| Field | Type | Meaning |
| :--- | :--- | :--- |
| `cmd` | string | One of the commands below. |
| `first_argument` | string | Command-specific — see the command table. |
| `second_argument` | string | Command-specific — see the command table. |
| `size` | uint32 | Whole-file size in bytes (`UPLOAD`); unused otherwise. |
| `file_hash` | string | Hex-encoded whole-file hash (`UPLOAD`); unused otherwise. |
| `chunks` | array of `ChunkInfo` | Per-chunk `{index, size, chunk_hash}` list (`UPLOAD`); omitted when empty. |

### Response (server → client)

```jsonc
{
  "status": "OK",
  "code": 200,
  "message": "Starting upload",
  "file_hash": "",
  "chunks": [],     // present only when non-empty (e.g. DOWNLOAD's chunk plan)
  "files": [],      // present only when non-empty (SYNC's directory listing)
  "tiers": []       // present only when non-empty (TIERS' storage medium listing)
}
```

| Field | Type | Meaning |
| :--- | :--- | :--- |
| `status` | string | One of the statuses below. |
| `code` | uint16 | HTTP-shaped status code — see "Status Codes". |
| `message` | string | Human-readable text; for `NEED_INPUT` responses this doubles as a machine-parsed hint (see below). |
| `file_hash` | string | Whole-file hash for `DOWNLOAD`; also carries a resume transfer id (as a decimal string) during resume negotiation. |
| `chunks` | array of `ChunkInfo` | `DOWNLOAD`'s chunk plan. |
| `files` | array of `FileEntry` | `SYNC`'s recursive listing of the requested remote directory. |
| `tiers` | array of `TierInfo` | `TIERS`' list of configured storage media. |

### Supporting types

```cpp
struct ChunkInfo {
    uint32_t index;
    uint32_t size;
    std::string chunk_hash;   // hex-encoded BLAKE2b of this chunk's bytes
};

struct FileEntry {             // one entry of a SYNC directory listing
    std::string relative_path; // '/'-separated, relative to the listed directory
    uint32_t size;             // 0 for directories
    std::string file_hash;     // hex-encoded whole-file hash, empty for directories
    uint64_t last_modified;    // seconds since epoch
    bool is_directory;
};

struct TierInfo {              // one storage medium (TIERS responses)
    std::string name;          // the SET_TIER argument
    std::string description;   // admin-supplied text
    bool is_current;           // true for the tier the calling user is on
};
```

## Statuses

| Status | Meaning |
| :--- | :--- |
| `OK` | Success. |
| `ERROR` | Failure; `code`/`message` explain why. |
| `AUTH` | The server wants a password next (in response to `LOGIN`). |
| `NEED_INPUT` | The server wants a `y`/`n` answer next (registration prompt, resume prompt, `SET_TIER` confirmation). |
| `CONFLICT` | Reserved for future two-way-conflict signaling at the protocol level (today's `SYNC` conflicts are resolved entirely client-side — see requirements.md). |
| `BUSY` | The user already has an operation in flight; retry later. |
| `EXIT` | The server is closing the connection. |
| `RESUME` | Server-initiated: either a resume question, or (when `file_hash` carries a transfer id) the kickoff of a specific resumable transfer. |

## Status Codes

Defined in `shared/include/protocol/codes.hpp`, HTTP-shaped for familiarity:

| Code | Name | Typical cause |
| :--- | :--- | :--- |
| 200 | OK | Success. |
| 202 | Accepted | A transfer was initiated. |
| 400 | Bad Request | Missing or malformed argument. |
| 401 | Unauthorized | Wrong password. |
| 403 | Forbidden | Operation not permitted (e.g. some operations in public mode). |
| 404 | Not Found | Path, user, or tier doesn't exist. |
| 409 | Conflict | — |
| 412 | Precondition Failed | Destination already exists, file already exists, file is empty, etc. |
| 500 | Internal Server Error | Unexpected I/O or server-side failure. |
| 503 | Service Unavailable | The calling user already has an operation in progress. |

## Chunk Flags

`ChunkHeader.flags`, defined in `shared/include/protocol/flags.hpp`:

| Flag | Value | Meaning |
| :--- | :--- | :--- |
| `OK` | 0 | Chunk accepted. |
| `ERROR` | 1 | Transfer failed; abort. |
| `CHUNK_MISMATCH` | 2 | Chunk hash/size didn't match the negotiated plan. |
| `LAST` | 3 | This is the final chunk of the file (sender → receiver). |
| `SEND` | 4 | An ordinary (non-final) chunk (sender → receiver). |
| `DONE` | 5 | Whole-file transfer completed and verified (receiver → sender ack on the last chunk). |
| `EXIT` | 6 | Sender is disconnecting mid-transfer. |

## Commands

`first_argument`/`second_argument` meanings by command (all commands are sent as a `Request`; see
[flows.md](flows.md) for the response/data-phase sequencing of `UPLOAD`/`DOWNLOAD`):

| `cmd` | `first_argument` | `second_argument` | Notes |
| :--- | :--- | :--- | :--- |
| `LOGIN` | username (empty for public mode) | — | Sent automatically on connect. |
| `AUTH` | password | — | Sent in response to an `AUTH`-status reply. |
| `NEED_INPUT` | `"y"` or `"n"` | — | Answers a registration/resume/`SET_TIER` prompt. |
| `LIST` | path (optional) | — | Current directory if omitted. |
| `UPLOAD` | local-relative destination path | — | Plus `size`/`file_hash`/`chunks`, see "Request". |
| `DOWNLOAD` | remote path | — | Response carries `file_hash` + `chunks` (the plan), then switches to the binary channel with the server sending. |
| `DELETE` | path | — | One call per path; the client loops for multi-path `DELETE`. |
| `CD` | path | — | |
| `MKDIR` | path | — | |
| `RMDIR` | path | — | Recursive. |
| `MOVE` | source path | destination path | One call per source; the client expands a multi-source/directory-target `MOVE` into several single calls. |
| `COPY` | source path | destination path | Same expansion as `MOVE`. |
| `SYNC` | remote directory to list | — | Stateless: returns a full recursive `files` listing; the client computes the diff and drives ordinary `UPLOAD`/`DOWNLOAD`/`DELETE`/`MOVE`/`COPY`/`MKDIR` calls itself. |
| `TIERS` | — | — | No user lock taken; returns `tiers`. |
| `SET_TIER` | tier name | — | Answered with `NEED_INPUT`; confirmed via a `NEED_INPUT` request. |
| `EXIT` | — | — | Either side may send it; the connection closes after. |

### Example: public-mode `LIST`

Request:
```json
{"cmd": "LIST", "first_argument": "", "second_argument": "", "size": 0, "file_hash": ""}
```
Response:
```json
{"status": "OK", "code": 200, "message": "Current directory: .\n", "file_hash": ""}
```

### Example: login → auth → ready

```
C: {"cmd":"LOGIN","first_argument":"alice", ...}
S: {"status":"AUTH","code":200,"message":"Please provide your password.", ...}
C: {"cmd":"AUTH","first_argument":"<password>", ...}
S: {"status":"OK","code":200,"message":"Authentication successful.", ...}
```

### Example: registration

```
C: {"cmd":"LOGIN","first_argument":"newuser", ...}
S: {"status":"NEED_INPUT","code":404,"message":"User newuser not found. Register? (y/n):", ...}
C: {"cmd":"NEED_INPUT","first_argument":"y", ...}
S: {"status":"AUTH","code":200,"message":"Choose a password.", ...}
C: {"cmd":"AUTH","first_argument":"<password>", ...}
S: {"status":"OK","code":200,"message":"Registration successful.", ...}
```

### Example: resume negotiation (private mode, after auth)

The kickoff response reuses `file_hash` (otherwise unused on a `RESUME` status) to carry the
transfer id as a decimal string, and `message` doubles as the human-readable prompt *and*, on the
question variant, a machine-parsed `"UPLOAD <path>"`/`"DOWNLOAD <path>"` string:

```
S: {"status":"RESUME","code":200,"message":"Incomplete upload/downloads detected, resume? (y/n):\nUPLOAD notes.txt","file_hash":""}
C: {"cmd":"NEED_INPUT","first_argument":"y", ...}
S: {"status":"RESUME","code":202,"message":"UPLOAD notes.txt","file_hash":"7"}   // "7" = transfer id
```
The connection then switches to the binary channel and resumes exactly like a fresh upload, except
the client only sends chunks the server doesn't already have.

### Example: `SYNC` listing

Request:
```json
{"cmd": "SYNC", "first_argument": "backups", "second_argument": "", "size": 0, "file_hash": ""}
```
Response:
```json
{
  "status": "OK", "code": 200, "message": "", "file_hash": "",
  "files": [
    {"relative_path": "notes.txt", "size": 1024, "file_hash": "a7d9bd...", "last_modified": 1734000000, "is_directory": false},
    {"relative_path": "photos", "size": 0, "file_hash": "", "last_modified": 1734000000, "is_directory": true}
  ]
}
```

### Example: storage tiering

```json
// TIERS request
{"cmd": "TIERS", "first_argument": "", "second_argument": "", "size": 0, "file_hash": ""}
// TIERS response
{"status": "OK", "code": 200, "message": "Available storage tiers:", "file_hash": "",
 "tiers": [{"name": "hot", "description": "NVMe SSD, fast", "is_current": true},
           {"name": "archive", "description": "7200rpm HDD, bulk", "is_current": false}]}
```
```
C: {"cmd":"SET_TIER","first_argument":"archive", ...}
S: {"status":"NEED_INPUT","code":200,"message":"Move your data to tier 'archive'? (y/n):", ...}
C: {"cmd":"NEED_INPUT","first_argument":"y", ...}
S: {"status":"OK","code":200,"message":"Moved to tier 'archive'.", ...}
```

### Errors

```json
{"status": "ERROR", "code": 412, "message": "File already exists.", "file_hash": ""}
```