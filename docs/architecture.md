# MiniDrive Architecture

## High-Level Components

- **Client (`client/`)**
  - Command-line interface with interactive shell and CLI parser.
  - State machine controlling athentification, ready, transfer and exit phases.
  - Transfer manager implementing chunked binary streaming over TCP.
  - Signal-aware shutdown logic ensuring graceful abort of active transfers.
  - Local file integrity validation (post-transfer hashing)
- **Server (`server/`)**
  - Listener accepting TCP connections using Asio with a thread pool.
  - Session manager controlling public/private roots and single-session limits.
  - Command dispatcher with handlers for file/folder operations and sync APIs.
  - Concurrent-safe transfer coordinator
  - Persistence layer storing users, hashes, and resumable transfer metadata.
  - Filesystem executor guarded against path traversal using `std::filesystem`.
  - Graceful shutdown logic ensuring graceful abort of active transfers
  - Password cryptographic helper `crypto_pwhash`.
- **Shared (`shared/`)**
  - JSON protocol schema and serialization helpers using `nlohmann::json`.
  - Error code definitions, commands, statuses, flags.
  - Cryptographic helpers leveraging `libsodium` for generic file hashes.
  - Filesystem helpers (path normalization, safety cheks).
  - Partial file metadata database manager

## Directory Layout

```
.
├── cmake                             # Toolchain and dependency helpers
├── CMakeLists.txt                    # Root build orchestrator
├── client                            
│   ├── CMakeLists.txt
│   ├── include
│   │   ├── client.hpp
│   │   └── terminalNoEcho.hpp        # Echo turn off/on
│   └── src
│       ├── client.cpp
│       ├── main.cpp
│       └── terminalNoEcho.cpp                                   
├── server
│   ├── CMakeLists.txt
│   ├── include
│   │   ├── database.hpp              # Database of users manager
│   │   ├── password.hpp              # Password cryptografic helper
│   │   ├── server.hpp
│   │   ├── session.hpp               # Session manager
│   │   └── storage.hpp               # Filesystem manager
│   └── src
│       ├── database.cpp
│       ├── main.cpp
│       ├── password.cpp
│       ├── server.cpp
│       ├── session.cpp
│       └── storage.cpp
├── shared
│   ├── CMakeLists.txt
│   ├── include
│   │   ├── filesystem                # Filesystem helpers 
│   │   ├── minidrive
│   │   └── protocol
│   └── src
│       ├── filesystem
│       ├── protocol
│       └── version.cpp
├── data
│   ├── client_root                   # Default root for client 
│   │   └── files
│   └── server_root                   # Default runtime root for server
│       ├── private
│       ├── public
│       └── users.json                # Database of users
├── tests
│   ├── CMakeLists.txt
│   └── integration
├── docs
│   ├── architecture.md
│   ├── protocol.md
│   └── requirements.md
└── README.md

```
