# MiniDrive Protocol

This document will capture the JSON command/response schema and binary transfer framing once the implementation stabilises.

## Control Channel

- All control messages are JSON documents encoded as UTF-8.
- Each message is framed using a 32-bit unsigned length prefix (network byte order).
- Example request:
  ```json
  { 
    "cmd": "UPLOAD", 
    "first_argument": "./data/client_root/files/file.txt", 
    "second_argument": "./test/file.txt", "size": "262144", 
    "file_hash": "f23cb2...", 
    "chunks": [ 
      { "index": "0", "size": "262144", "chunk_hash": "a7d9bd..." } 
    ] 
  }
  ```
- Example response:
  ```json
  { "status": "OK", 
    "code": 200, 
    "message": "Starting upload", 
    "file_hash": "", 
    "chunks": [
    ] 
  }
  ```

## Data Channel

File uploads/downloads reuse the TCP connection and stream binary chunks with per-chunk metadata (transfer_id, index, size, flags).
