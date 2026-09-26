# Desktop app (`gui/`)

MiniDrive's desktop app is a window onto the MiniDrive client. It browses and transfers files,
syncs folders, moves your data between storage tiers, and manages end-to-end encryption, on Linux,
Windows and macOS.

The app has no network protocol, TLS stack or cryptography of its own. It bundles the ordinary
MiniDrive client, starts it in headless mode (`--ipc`, see [ipc.md](ipc.md)), and drives it. The
wire protocol, TLS with pinning, the SYNC engine, resume, and the vault's whole key hierarchy all
run inside that client, exactly as they do on the command line. The app is a view over it.

## Architecture

```
 ┌──────────────────────── the app process ─────────────────────────┐
 │  React frontend  ── invoke() / events ──▶  Rust core (Tauri)      │
 │  file manager, dialogs,                    starts the client,     │
 │  progress, settings                        owns the socket,       │
 │                                            serializes commands    │
 └──────────────────────────────────────────────────┬───────────────┘
                                    Unix socket / named pipe, framed JSON
                                                    │
                                     ┌──────────────▼──────────────┐
                                     │ minidrive-client --ipc …     │  ── TLS 1.3 ──▶ server
                                     │ (the same binary as the CLI) │
                                     └──────────────────────────────┘
```

- **Frontend** (`gui/src`): React, built around
  [`@cubone/react-file-manager`](https://github.com/Saifullah-dev/react-file-manager) for the file
  grid, breadcrumbs, context menus and selection. It never builds a command line: it sends typed
  operations (`{op: "upload", local, remote}`) to the core.
- **Rust core** (`gui/src-tauri`): starts the bundled client for each session and turns operations
  into command lines. Every argument is quoted here, in one place. The core then runs the
  conversation, as below.
- **Client**: the headless MiniDrive client, bundled with the app through Tauri's `externalBin`.
  Release builds bundle the release's fully static client, so the app carries OpenSSL 3.5 inside
  the client and needs nothing from the host except its webview.

### The conversation

The client is strictly conversational. It takes one line, does the work, and then emits exactly
one `PROMPT` saying what it wants next: a command, the password, or a yes/no answer. The core
therefore runs one *exchange* at a time: send a line, collect every frame until the next `PROMPT`.
Two rules are enforced in `session.rs`, not left to the frontend:

- **One exchange at a time.** A second command waits for the first to finish rather than being
  queued inside the client, where it could be read as the answer to a question.
- **The right kind of line for the prompt.** A command is only sent while the client is at a
  command prompt, and an answer only while it is asking one. A folder refresh can never answer
  "Move your data to archive?".

When the client stops at a question (register this account? resume this interrupted upload?
migrate tiers? revoke this device?), the frontend shows the server's own question in a dialog and
sends the answer. An operation counts as finished only when the client is back at a command
prompt. Frames are also streamed live while an exchange runs, which is what drives the progress
bar and the activity log during a long upload or sync.

## Security properties

- **Only the client the app started can take the channel.** On Linux and macOS the socket lives in
  a directory created fresh with mode `0700`. On Windows the pipe has a random 128-bit name, a single
  instance, and refuses remote clients. After connecting, the peer's process id must be the child
  the session spawned (`SO_PEERCRED`/`LOCAL_PEERPID`, or `GetNamedPipeClientProcessId`). If another
  process races the real client in, it is refused and the session fails to start rather than
  being handed over.
- **The password is typed in the app and goes only to the client.** It is not stored in any
  profile. The client never echoes it back, and the core wipes its copies once they are written to
  the socket. The client's own log redacts it (`AUTH ***`).
- **The TLS posture shown is the one negotiated, not the one configured.** The header badge is
  parsed from the client's report of the live connection (`Secure connection: TLSv1.3, cipher …,
  group X25519MLKEM768`). An unencrypted connection is labelled **Not encrypted** in red.
- **Verification cannot be turned off from the app.** A profile offers TLS with a CA file, a pin,
  or both, but never `--tls-verify none`. A CA file or pin on a plaintext profile is refused rather
  than silently ignored.
- **Each account gets its own state directory**, under the app's data directory
  (`accounts/<user>@<host>_<port>/`). It holds the client's partial-transfer records, vault scratch
  files, this device's vault keys (mode 0600) and the client log. Two accounts, or two servers, never
  share resume state.
- The webview runs under a restrictive content security policy, with Tauri's core default
  permissions and the native *open* dialog, and nothing else. It gets no shell, filesystem or HTTP
  plugin. Every MiniDrive action goes through the app's own nine commands.

## Using it

**Servers.** Add a server with its address, port and username. Leave the username empty for the
shared public area. Under *Security*, keep TLS on and give either the server's CA certificate (the
`ca.crt` from `lab/gen-certs.sh`) or its public-key pin (`sha256:…`, printed by the server at
startup and saved as `server.pin`). Giving both is stricter. *Require post-quantum key exchange*
makes the connection fail rather than fall back to a classical group.

**Files.** Browse by double-clicking folders or using the breadcrumbs. *Upload files* and *Upload
folder* open the system file picker, and you can also drop files and folders from the desktop
anywhere on the window. They go to the folder being shown. Uploading onto an existing name asks
before replacing it. Rename, delete, cut/copy/paste and *New Folder* are in the toolbar and the
right-click menu. *Download* asks where to save.

**Sync.** Pair a local folder with a server folder, then *Sync now*. This is the client's
three-way sync: changes flow both ways, deletions propagate, and when both sides changed the same
file the server's version is saved beside yours as a conflict copy, so nothing is lost. The pairs
are remembered per server.

**Storage.** Lists the server's storage tiers. *Move my files here* migrates your data: the server
asks for confirmation, copies, verifies, and only then removes the original.

**Vault.** Turns on end-to-end encryption for the account. There is **no recovery**: if you lose
the password and have no enrolled device, the encrypted files cannot be opened by anyone. Enrolling
a device gives this computer its own post-quantum key to the vault, and you can revoke devices
here. See [vault.md](vault.md).

**Activity.** Everything the client reported this session, with errors highlighted. The client's
own log is `client.log` in the account's state directory.

**Stopping a transfer.** The protocol has no per-transfer cancel, so *Stop* ends the session. An
interrupted transfer is saved, and the next sign-in offers to resume it. A transfer resumed at
sign-in shows its progress on the connect screen, and *Cancel* there stops it the same way.

## Limitations

- **Installers are not code-signed.** macOS Gatekeeper and Windows SmartScreen will warn. On macOS,
  open the app with right-click → *Open* the first time.
- **On Windows, dragging items within the file list is expected not to move them.** The window
  accepts OS file drops, because those carry real paths, and Tauri documents that this disables
  in-page drag-and-drop on Windows. Use cut and paste instead. (Not yet observed on Windows.)
- **Empty files cannot be uploaded.** This is a limitation of the transfer protocol, and the app
  says so rather than trying.
- **Peer-approval device enrollment** (a new device shows a code, an enrolled one approves it, no
  password typed) is not built yet. Devices enroll with the password, as on the command line.
- One session per app window. Signing in to the same account from two app instances at once works,
  but the two instances share that account's state directory.
- A folder that contains a name with a newline or other control character cannot be addressed. The
  core refuses rather than send a command the client would split.

## Building

Prerequisites: Node 20+, a stable Rust toolchain, a built MiniDrive client, and Tauri's platform
prerequisites (on Debian/Ubuntu: `libwebkit2gtk-4.1-dev libxdo-dev libayatana-appindicator3-dev
librsvg2-dev`; the dev container has them).

```sh
cmake --build build --target minidrive_client   # the sidecar
cd gui
npm ci
npm run stage-sidecar       # copies build/client/client to src-tauri/binaries/minidrive-client-<triple>
npx tauri dev               # development, with hot reload
npx tauri build             # installers for this platform
```

`stage-sidecar` takes an explicit path and `--target <triple>` for other builds. It honours
`MINIDRIVE_BUILD_DIR`. Tauri requires the target triple in the staged name, and strips it when
bundling, so at runtime the client sits beside the app as `minidrive-client`. Debug builds also
accept `MINIDRIVE_CLIENT_BIN=<path>` to run a client from elsewhere.

## Testing

| Layer | Command | What it covers |
|---|---|---|
| Frontend | `npm test` | path mapping, tree merging, TLS posture parsing, question detection |
| Rust core | `cargo test --lib` (in `src-tauri`) | framing, frame parsing, quoting checked against a model of the client's reader, profiles, endpoint privacy and peer-process refusal |
| Core against the real binaries | `cargo test --test live` | awkward filenames through every file operation, serialized concurrency, registration → password → vault → confirmed tier migration, stopping mid-transfer and resuming, pinned TLS vs. a wrong pin |
| The app itself | `python3 gui/tests/e2e.py` | the built window driven through WebDriver under Xvfb: sign-up dialogs, the posture badge, browsing, drop-to-upload with replace, turning the vault on (and checking the server holds ciphertext), tiers, activity, wrong password, wrong pin |

The live tests look for the binaries in `$MINIDRIVE_BUILD_DIR` (default `build/`) and skip with
a message if they are missing. Tests that need no server also run on Windows and macOS, and CI
runs them there. The end-to-end test needs a debug build with the frontend embedded
(`npx tauri build --debug --no-bundle`), `tauri-driver` (`cargo install tauri-driver`),
WebKitWebDriver (`webkit2gtk-driver`) and Xvfb. It saves screenshots under `test_logs/gui_e2e_*`.
When running it inside the dev container, the forwarded Wayland socket would otherwise take the
window, so the script forces `GDK_BACKEND=x11`.
