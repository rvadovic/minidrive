# Transfer Protocol Flow

A transfer process consists of a JSON control phase follewed by binary data phase.


## Upload

### Control Phase
1. Client sends `UPLOAD` request with metadata and `ChunkInfo` vector.
2. Server validates request, responds with `OK`. Both server and client set their `state_` to `UPLOADING` which also switches the protocol.

### Data Phase
- Client sends binary chunks with metadata headers (transfer_id, index, size, flags)
- Server validates each chunk using generic hash and ancknowledges by sending the same metdata header (transfer_id, index, size, flags) and signaling reponse in flag.
- Final integrity is verified using a full-file generic hash.


## Download

### Control Phase
1. Client sends `DOWNLOAD` request with requested file.
2. Server validates request, responds with `OK`, requested file metadata, `ChunkInfo` vector and sets its `state_` to `DOWNLOADING`.
3. Client prepares for download and sets its `state_` to `DOWNLOADING`.

### Data Phase
- Server sends binary chunks with metadata headers (transfer_id, index, size, flags)
- Client validates each chunk using generic hash and ancknowledges by sending the same metdata header (transfer_id, index, size, flags) and signaling reponse in flag.
- Final integrity is verified using a full-file generic hash.