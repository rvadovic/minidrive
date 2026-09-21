## Downloads

The **server** and the **client** are separate downloads.

| You want | Download |
|---|---|
| The server, on Debian 13 | `minidrive-server-<version>-debian13-amd64.deb`, installed with `sudo apt install ./minidrive-server-*.deb` |
| To build the server yourself | `minidrive-server-<version>-src.tar.gz`. Follow `BUILD-SERVER.md` inside it (the dev container is included) |
| The client on Linux (any x86_64 distribution) | `minidrive-client-<version>-linux-x86_64.tar.gz`. A single static binary with the interactive command line |
| The client on Windows | `minidrive-client-<version>-windows-x86_64.zip` (**headless**, see below) |
| The client on macOS | `minidrive-client-<version>-macos-arm64.tar.gz` (Apple silicon) or `-macos-x86_64.tar.gz` (Intel) (**headless**, see below) |

`SHA256SUMS` lists checksums for all of the above. Check them with `sha256sum -c SHA256SUMS --ignore-missing`.

**The Windows and macOS clients are headless.** They run only with `--ipc <endpoint>`, driven by
another program over a named pipe (Windows) or a Unix socket (macOS). That mode exists for the
upcoming desktop GUI, and neither build includes an interactive command line. See `docs/ipc.md`.
For an interactive client, use the Linux build.

**Clients have no dependencies.** Each one embeds OpenSSL 3.5 LTS, so every client supports the
hybrid post-quantum TLS group `X25519MLKEM768` and vault device enrollment, whatever the host
provides. Because OpenSSL is embedded, an OpenSSL security fix ships as a new client release;
upgrade the client when one appears. The server links the system OpenSSL, which Debian's security
updates keep patched.
