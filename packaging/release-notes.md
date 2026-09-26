## Downloads

The **server**, the **desktop app** and the **command-line client** are separate downloads.

| You want | Download |
|---|---|
| **The desktop app** on Windows | `minidrive-gui-<version>-windows-x86_64.msi` |
| **The desktop app** on macOS | `minidrive-gui-<version>-macos-arm64.dmg` (Apple silicon) or `-macos-x86_64.dmg` (Intel) |
| **The desktop app** on Linux | `minidrive-gui-<version>-linux-amd64.deb` (Debian/Ubuntu) or `-linux-x86_64.AppImage` (any distribution) |
| The server, on Debian 13 | `minidrive-server-<version>-debian13-amd64.deb`, installed with `sudo apt install ./minidrive-server-*.deb` |
| To build the server yourself | `minidrive-server-<version>-src.tar.gz`. Follow `BUILD-SERVER.md` inside it (the dev container is included) |
| The client on Linux (any x86_64 distribution) | `minidrive-client-<version>-linux-x86_64.tar.gz`. A single static binary with the interactive command line |
| The client on Windows | `minidrive-client-<version>-windows-x86_64.zip` (**headless**, see below) |
| The client on macOS | `minidrive-client-<version>-macos-arm64.tar.gz` (Apple silicon) or `-macos-x86_64.tar.gz` (Intel) (**headless**, see below) |

`SHA256SUMS` lists checksums for all of the above. Check them with `sha256sum -c SHA256SUMS --ignore-missing`.

**The desktop app is the way to use MiniDrive on Windows and macOS.** It bundles this release's
client and drives it, so it has the same TLS, pinning, sync, resume and end-to-end encryption as
the command line. See `docs/gui.md`. **The installers are not code-signed**, so Windows SmartScreen
and macOS Gatekeeper will warn the first time. On macOS, right-click the app and choose *Open*.

**The Windows and macOS clients are headless.** They run only with `--ipc <endpoint>`, driven by
another program over a named pipe (Windows) or a Unix socket (macOS). This is the mode the desktop
app uses, and neither build includes an interactive command line. See `docs/ipc.md`. For an
interactive command line, use the Linux build.

**Clients have no dependencies.** Each one embeds OpenSSL 3.5 LTS, so every client supports the
hybrid post-quantum TLS group `X25519MLKEM768` and vault device enrollment, whatever the host
provides. Because OpenSSL is embedded, an OpenSSL security fix ships as a new client release;
upgrade the client when one appears. The server links the system OpenSSL, which Debian's security
updates keep patched.
