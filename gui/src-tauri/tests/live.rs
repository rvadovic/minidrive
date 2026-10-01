//! The GUI core against the real thing: a real `server` and the real `client` binary, driven
//! through exactly the `Session` + `Operation` path the Tauri commands use.
//!
//! Needs a built tree (`cmake --build build`). Uses `$MINIDRIVE_BUILD_DIR` if set, else
//! `<repo>/build`. When the binaries are missing each test says so and passes vacuously, so
//! `cargo test` still works on a checkout that has only built the GUI; CI builds them first.

use std::io::Write;
use std::net::TcpListener;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use minidrive_gui_lib::messages::{Frame, PromptKind};
use minidrive_gui_lib::ops::Operation;
use minidrive_gui_lib::profile::{Profile, Security};
use minidrive_gui_lib::session::{Ending, Exchange, Launch, Observer, Session, SessionError};
use serde_json::Value;

fn build_dir() -> PathBuf {
    std::env::var_os("MINIDRIVE_BUILD_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| Path::new(env!("CARGO_MANIFEST_DIR")).join("../../build"))
}

/// `<build>/<target>/<name>`, or `<build>/<target>/Release/<name>` from a multi-config generator.
fn built(target: &str, name: &str) -> Option<PathBuf> {
    let file = format!("{name}{}", std::env::consts::EXE_SUFFIX);
    let dir = build_dir().join(target);
    [dir.join(&file), dir.join("Release").join(&file)].into_iter().find(|p| p.is_file())
}

fn client_binary() -> Option<PathBuf> {
    let client = built("client", "client");
    if client.is_none() {
        eprintln!("skipping: no client binary under {}", build_dir().display());
    }
    client
}

/// The server is Linux-only, so tests that need one skip elsewhere.
fn binaries() -> Option<(PathBuf, PathBuf)> {
    match (built("server", "server"), built("client", "client")) {
        (Some(server), Some(client)) => Some((server, client)),
        _ => {
            eprintln!("skipping: no server/client binaries under {}", build_dir().display());
            None
        }
    }
}

fn free_port() -> u16 {
    TcpListener::bind("127.0.0.1:0").unwrap().local_addr().unwrap().port()
}

struct Server {
    child: Child,
    port: u16,
    root: tempfile::TempDir,
}

impl Server {
    fn start(binary: &Path, extra: &[&str]) -> Server {
        Server::start_in(binary, tempfile::tempdir().unwrap(), extra)
    }

    fn start_in(binary: &Path, root: tempfile::TempDir, extra: &[&str]) -> Server {
        let port = free_port();
        let child = Command::new(binary)
            .args(["--port", &port.to_string(), "--root"])
            .arg(root.path())
            .args(extra)
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .unwrap();
        let deadline = Instant::now() + Duration::from_secs(10);
        while std::net::TcpStream::connect(("127.0.0.1", port)).is_err() {
            assert!(Instant::now() < deadline, "server did not start listening");
            std::thread::sleep(Duration::from_millis(50));
        }
        Server { child, port, root }
    }

    fn public_file(&self, relative: &str) -> PathBuf {
        self.root.path().join("public").join("files").join(relative)
    }

    fn private_file(&self, user: &str, relative: &str) -> PathBuf {
        self.root.path().join("private").join(user).join("files").join(relative)
    }
}

impl Drop for Server {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

#[derive(Default)]
struct Recorder {
    frames: Mutex<Vec<Frame>>,
    endings: Mutex<Vec<Ending>>,
}

impl Observer for Recorder {
    fn frame(&self, _session: u64, frame: &Frame) {
        self.frames.lock().unwrap().push(frame.clone());
    }
    fn ended(&self, _session: u64, ending: &Ending) {
        self.endings.lock().unwrap().push(ending.clone());
    }
}

impl Recorder {
    fn events(&self, name: &str) -> Vec<Value> {
        self.frames
            .lock()
            .unwrap()
            .iter()
            .filter_map(|f| match f {
                Frame::Event { name: n, data } if n == name => Some(data.clone()),
                _ => None,
            })
            .collect()
    }
}

fn profile(port: u16, username: &str) -> Profile {
    Profile {
        id: "test".into(),
        name: "test".into(),
        host: "127.0.0.1".into(),
        port,
        username: username.into(),
        security: Security::default(),
        sync_pairs: vec![],
        download_dir: None,
    }
}

async fn start(client: &Path, profile: &Profile, state: &Path, recorder: Arc<Recorder>) -> Result<(Arc<Session>, Exchange), SessionError> {
    let launch = Launch {
        program: client.to_path_buf(),
        args: profile.client_args(state),
        working_dir: state.to_path_buf(),
    };
    Session::start(1, launch, recorder).await
}

async fn run(session: &Session, op: Operation) -> Exchange {
    let exchange = session.command(&op.to_line().unwrap()).await.unwrap();
    assert!(!exchange.ended(), "session ended during {op:?}: {:?}", exchange.frames);
    exchange
}

fn ok(exchange: &Exchange) -> bool {
    exchange.last_result().map(|(ok, _, _)| ok).unwrap_or(false)
}

fn listing(exchange: &Exchange) -> Vec<(String, bool, u64)> {
    exchange
        .frames
        .iter()
        .find_map(|f| match f {
            Frame::Event { name, data } if name == "listing" => Some(data["entries"].as_array().unwrap().clone()),
            _ => None,
        })
        .expect("no listing event")
        .iter()
        .map(|e| (e["name"].as_str().unwrap().to_owned(), e["is_directory"].as_bool().unwrap(), e["size"].as_u64().unwrap()))
        .collect()
}

fn write_random(path: &Path, len: usize) -> Vec<u8> {
    let mut data = vec![0u8; len];
    let mut x: u64 = 0x9e37_79b9_7f4a_7c15 ^ len as u64;
    for byte in data.iter_mut() {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        *byte = x as u8;
    }
    std::fs::File::create(path).unwrap().write_all(&data).unwrap();
    data
}

#[tokio::test]
async fn file_manager_operations_round_trip_with_awkward_names() {
    let Some((server_bin, client_bin)) = binaries() else { return };
    let server = Server::start(&server_bin, &[]);
    let state = tempfile::tempdir().unwrap();
    let work = tempfile::tempdir().unwrap();
    let recorder = Arc::new(Recorder::default());

    let (session, first) = start(&client_bin, &profile(server.port, ""), state.path(), recorder.clone()).await.unwrap();
    assert_eq!(first.prompt.as_ref().unwrap().kind, PromptKind::Command, "public mode goes straight to commands");

    assert!(ok(&run(&session, Operation::Mkdir { path: "/My Folder".into() }).await));

    let source = work.path().join("report \"final\" v2.bin");
    let data = write_random(&source, 700 * 1024);
    let upload = run(&session, Operation::Upload { local: source.to_string_lossy().into(), remote: "/My Folder/report \"final\" v2.bin".into() }).await;
    assert!(ok(&upload), "{:?}", upload.frames);
    assert_eq!(std::fs::read(server.public_file("My Folder/report \"final\" v2.bin")).unwrap(), data);

    let progress = recorder.events("transfer_progress");
    assert!(!progress.is_empty(), "the observer saw no live progress");
    assert_eq!(progress.last().unwrap()["remote_path"], "/My Folder/report \"final\" v2.bin");

    let entries = listing(&run(&session, Operation::List { path: "/My Folder".into() }).await);
    assert_eq!(entries, vec![("report \"final\" v2.bin".into(), false, 700 * 1024)]);

    // Rename, then copy into a new folder - the file manager's rename and paste.
    assert!(ok(&run(&session, Operation::Move {
        sources: vec!["/My Folder/report \"final\" v2.bin".into()],
        destination: "/My Folder/renamed file.bin".into(),
        into_directory: false,
    }).await));
    assert!(ok(&run(&session, Operation::Mkdir { path: "/Other Place".into() }).await));
    let copy = run(&session, Operation::Copy {
        sources: vec!["/My Folder/renamed file.bin".into()],
        destination: "/Other Place".into(),
        into_directory: true,
    }).await;
    assert!(ok(&copy), "{:?}", copy.frames);
    assert!(server.public_file("Other Place/renamed file.bin").is_file());

    let fetched = work.path().join("fetched back.bin");
    let download = run(&session, Operation::Download { remote: "/Other Place/renamed file.bin".into(), local: fetched.to_string_lossy().into() }).await;
    assert!(ok(&download), "{:?}", download.frames);
    assert_eq!(std::fs::read(&fetched).unwrap(), data);

    assert!(ok(&run(&session, Operation::Delete { paths: vec!["/Other Place/renamed file.bin".into()] }).await));
    assert!(ok(&run(&session, Operation::Rmdir { path: "/My Folder".into() }).await));
    let root = listing(&run(&session, Operation::List { path: "/".into() }).await);
    assert_eq!(root, vec![("Other Place".into(), true, 0)]);

    let ending = session.close().await;
    assert_eq!(ending.exit_code, Some(0));
    assert!(state.path().join(".partial").join("partmeta.json").is_file(), "--state-dir was not honoured");
}

#[tokio::test]
async fn a_line_of_the_wrong_kind_for_the_prompt_is_refused() {
    let Some((server_bin, client_bin)) = binaries() else { return };
    let server = Server::start(&server_bin, &[]);
    let state = tempfile::tempdir().unwrap();
    let (session, _) = start(&client_bin, &profile(server.port, ""), state.path(), Arc::new(Recorder::default())).await.unwrap();

    // At a command prompt, an "answer" must not be sent: it would be run as a command.
    assert!(matches!(session.respond("y").await, Err(SessionError::WrongPrompt { expected: PromptKind::Command, .. })));
    session.close().await;
}

#[tokio::test]
async fn concurrent_commands_are_serialized_not_interleaved() {
    let Some((server_bin, client_bin)) = binaries() else { return };
    let server = Server::start(&server_bin, &[]);
    let state = tempfile::tempdir().unwrap();
    let (session, _) = start(&client_bin, &profile(server.port, ""), state.path(), Arc::new(Recorder::default())).await.unwrap();

    let mut tasks = Vec::new();
    for i in 0..6 {
        let session = session.clone();
        tasks.push(tokio::spawn(async move {
            session.command(&Operation::Mkdir { path: format!("/dir {i}") }.to_line().unwrap()).await.unwrap()
        }));
    }
    for task in tasks {
        let exchange = task.await.unwrap();
        // Without serialization the client would answer "Server is busy..." to the overlap.
        assert!(ok(&exchange), "{:?}", exchange.frames);
    }
    assert_eq!(listing(&run(&session, Operation::List { path: "/".into() }).await).len(), 6);
    session.close().await;
}

#[tokio::test]
async fn registration_password_vault_and_confirmed_tier_change() {
    let Some((server_bin, client_bin)) = binaries() else { return };
    // Two tiers; the control root doubles as "hot".
    let archive = tempfile::tempdir().unwrap();
    let root = tempfile::tempdir().unwrap();
    let tier_hot = format!("hot={}", root.path().display());
    let tier_archive = format!("archive={}", archive.path().display());
    let server = Server::start_in(&server_bin, root, &["--tier", &tier_hot, "--tier", &tier_archive, "--default-tier", "hot"]);

    let state = tempfile::tempdir().unwrap();
    let work = tempfile::tempdir().unwrap();
    let (session, first) = start(&client_bin, &profile(server.port, "gui_alice"), state.path(), Arc::new(Recorder::default())).await.unwrap();

    let question = first.prompt.unwrap();
    assert_eq!(question.kind, PromptKind::Confirm, "a new user is asked whether to register");
    assert!(!question.text.is_empty(), "the confirm prompt carries its question");
    assert!(matches!(session.command("LIST").await, Err(SessionError::WrongPrompt { .. })), "a command cannot answer a question");

    let password = session.respond("y").await.unwrap().prompt.unwrap();
    assert_eq!(password.kind, PromptKind::Password);
    let authed = session.respond("correct horse battery").await.unwrap();
    assert_eq!(authed.prompt.unwrap().kind, PromptKind::Command);

    let init = session.command("VAULT_INIT").await.unwrap();
    assert!(ok(&init), "{:?}", init.frames);
    let status = run(&session, Operation::VaultStatus).await;
    let vault = status.frames.iter().find_map(|f| match f {
        Frame::Event { name, data } if name == "vault_status" => Some(data.clone()),
        _ => None,
    }).unwrap();
    assert_eq!(vault["unlocked"], true);

    let source = work.path().join("sealed.bin");
    let data = write_random(&source, 600 * 1024);
    assert!(ok(&run(&session, Operation::Upload { local: source.to_string_lossy().into(), remote: "/sealed.bin".into() }).await));
    let stored = std::fs::read(server.private_file("gui_alice", "sealed.bin")).unwrap();
    assert_ne!(stored, data, "the server must hold ciphertext");
    assert_eq!(listing(&run(&session, Operation::List { path: "/".into() }).await), vec![("sealed.bin".into(), false, 600 * 1024)]);

    // SET_TIER asks for confirmation; the exchange stops at the question, and the answer finishes it.
    let asked = run(&session, Operation::SetTier { tier: "archive".into() }).await;
    assert_eq!(asked.prompt.as_ref().unwrap().kind, PromptKind::Confirm);
    let moved = session.respond("y").await.unwrap();
    assert!(ok(&moved), "{:?}", moved.frames);
    let tiers = run(&session, Operation::Tiers).await;
    let current: Vec<String> = tiers.frames.iter().find_map(|f| match f {
        Frame::Event { name, data } if name == "tiers" => Some(data["tiers"].as_array().unwrap().clone()),
        _ => None,
    }).unwrap().iter().filter(|t| t["current"] == true).map(|t| t["name"].as_str().unwrap().to_owned()).collect();
    assert_eq!(current, vec!["archive"]);
    assert!(archive.path().join("private/gui_alice/files/sealed.bin").is_file());

    // The file still opens after the migration: the vault's manifest moved with it.
    let back = work.path().join("back.bin");
    assert!(ok(&run(&session, Operation::Download { remote: "/sealed.bin".into(), local: back.to_string_lossy().into() }).await));
    assert_eq!(std::fs::read(&back).unwrap(), data);
    session.close().await;
}

#[tokio::test]
async fn closing_mid_transfer_leaves_it_resumable() {
    let Some((server_bin, client_bin)) = binaries() else { return };
    let server = Server::start(&server_bin, &[]);
    let state = tempfile::tempdir().unwrap();
    let work = tempfile::tempdir().unwrap();
    let source = work.path().join("big.bin");
    let data = write_random(&source, 96 * 1024 * 1024);

    // Register first, in its own session.
    let (session, _) = start(&client_bin, &profile(server.port, "gui_bob"), state.path(), Arc::new(Recorder::default())).await.unwrap();
    session.respond("y").await.unwrap();
    session.respond("bobs password").await.unwrap();
    session.close().await;

    let recorder = Arc::new(Recorder::default());
    let (session, _) = start(&client_bin, &profile(server.port, "gui_bob"), state.path(), recorder.clone()).await.unwrap();
    session.respond("bobs password").await.unwrap();

    let line = Operation::Upload { local: source.to_string_lossy().into(), remote: "/big.bin".into() }.to_line().unwrap();
    let uploading = {
        let session = session.clone();
        tokio::spawn(async move { session.command(&line).await })
    };
    // Close only once chunks are actually flowing and not yet finished.
    let deadline = Instant::now() + Duration::from_secs(60);
    loop {
        let progress = recorder.events("transfer_progress");
        if progress.iter().any(|p| p["chunks_done"].as_u64().unwrap() > 0 && p["chunks_done"] != p["chunks_total"]) {
            break;
        }
        assert!(Instant::now() < deadline, "the upload never started");
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
    let ending = session.close().await;
    assert_eq!(ending.exit_code, Some(0), "closing the channel must let the client save state and exit");
    let interrupted = uploading.await.unwrap().unwrap();
    assert!(interrupted.ended(), "the running exchange ends with the session");
    assert!(!server.private_file("gui_bob", "big.bin").exists(), "closed too late to test anything");

    // The next login offers the resume; accepting it finishes the same upload.
    let (session, _) = start(&client_bin, &profile(server.port, "gui_bob"), state.path(), Arc::new(Recorder::default())).await.unwrap();
    let offer = session.respond("bobs password").await.unwrap();
    let question = offer.prompt.unwrap();
    assert_eq!(question.kind, PromptKind::Confirm);
    assert!(question.text.contains("resume"), "{:?}", question.text);
    let resumed = session.respond("y").await.unwrap();
    assert!(resumed.frames.iter().any(|f| matches!(f, Frame::Result { ok: true, message, .. } if message.contains("Upload successful"))), "{:?}", resumed.frames);
    assert_eq!(std::fs::read(server.private_file("gui_bob", "big.bin")).unwrap(), data);
    session.close().await;
}

#[tokio::test]
async fn pinned_tls_reports_its_posture_and_a_wrong_pin_is_refused() {
    let Some((server_bin, client_bin)) = binaries() else { return };
    let certs = tempfile::tempdir().unwrap();
    let gen = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../lab/gen-certs.sh");
    let status = Command::new("bash")
        .arg(&gen)
        .args(["init", "--lab"])
        .arg(certs.path())
        .env("MINIDRIVE_CA_PASSPHRASE", "live-test-throwaway")
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status();
    if !matches!(status, Ok(s) if s.success()) {
        eprintln!("skipping: lab/gen-certs.sh unavailable (needs bash and openssl)");
        return;
    }
    let cert = certs.path().join("server/server.crt");
    let key = certs.path().join("server/server.key");
    let server = Server::start(&server_bin, &["--rung", "3.5", "--tls-cert", cert.to_str().unwrap(), "--tls-key", key.to_str().unwrap()]);
    let pin = std::fs::read_to_string(certs.path().join("server/server.pin")).unwrap().trim().to_owned();

    let mut good = profile(server.port, "");
    good.security = Security { tls: true, pin: Some(pin), ..Security::default() };
    let state = tempfile::tempdir().unwrap();
    let (session, first) = start(&client_bin, &good, state.path(), Arc::new(Recorder::default())).await.unwrap();
    let posture = first.frames.iter().find_map(|f| match f {
        Frame::Result { message, .. } if message.starts_with("Secure connection:") => Some(message.clone()),
        _ => None,
    });
    assert!(posture.as_deref().is_some_and(|p| p.contains("TLSv1.3")), "{posture:?}");
    assert_eq!(first.prompt.unwrap().kind, PromptKind::Command);
    session.close().await;

    let mut bad = profile(server.port, "");
    let wrong = std::fs::read_to_string(certs.path().join("adversary/rogue-server.pin")).unwrap().trim().to_owned();
    bad.security = Security { tls: true, pin: Some(wrong), ..Security::default() };
    let state = tempfile::tempdir().unwrap();
    let (session, first) = start(&client_bin, &bad, state.path(), Arc::new(Recorder::default())).await.unwrap();
    assert!(first.ended(), "a wrong pin must not reach a prompt: {:?}", first.frames);
    let ending = session.wait_finished().await;
    assert!(ending.stderr.iter().any(|l| l.contains("TLS handshake failed")), "{ending:?}");
}

/// Needs no server, so it runs on every OS - including Windows, where it is the check that the
/// named-pipe endpoint, the peer-process check and the framing all work against the real client.
#[tokio::test]
async fn an_unreachable_server_is_reported_over_the_channel() {
    let Some(client_bin) = client_binary() else { return };
    let state = tempfile::tempdir().unwrap();
    let recorder = Arc::new(Recorder::default());
    let (session, first) = start(&client_bin, &profile(free_port(), ""), state.path(), recorder.clone()).await.unwrap();

    assert!(first.ended(), "nothing is listening, so no prompt can come: {:?}", first.frames);
    assert!(
        first.frames.iter().any(|f| matches!(f, Frame::Result { ok: false, .. })),
        "the failure reached the host as a frame: {:?}",
        first.frames
    );
    let ending = session.wait_finished().await;
    assert!(ending.last_error.is_some(), "{ending:?}");
    assert_eq!(recorder.endings.lock().unwrap().len(), 1, "the observer hears the ending exactly once");
}

/// A cancelled connect must not leave the client running: a server that accepts the TCP connection
/// and then says nothing keeps the first exchange waiting forever, and abandoning it has to close the
/// channel so the client exits on its own.
#[tokio::test]
async fn abandoning_a_hanging_connect_stops_the_client() {
    let Some(client_bin) = client_binary() else { return };
    let silent = TcpListener::bind("127.0.0.1:0").unwrap(); // accepts into the backlog, never replies
    let port = silent.local_addr().unwrap().port();
    let state = tempfile::tempdir().unwrap();
    let recorder = Arc::new(Recorder::default());

    let attempt = tokio::time::timeout(
        Duration::from_secs(2),
        start(&client_bin, &profile(port, ""), state.path(), recorder.clone()),
    )
    .await;
    assert!(attempt.is_err(), "the silent server should have kept the connect waiting");

    let deadline = Instant::now() + Duration::from_secs(15);
    while recorder.endings.lock().unwrap().is_empty() {
        assert!(Instant::now() < deadline, "the abandoned client is still running");
        tokio::time::sleep(Duration::from_millis(50)).await;
    }
    assert_eq!(recorder.endings.lock().unwrap()[0].exit_code, Some(0), "it should exit cleanly, not be killed");
    drop(silent);
}

#[tokio::test]
async fn a_client_that_cannot_start_is_reported_with_its_reason() {
    let Some(client_bin) = client_binary() else { return };
    let state = tempfile::tempdir().unwrap();
    let launch = Launch {
        program: client_bin,
        args: vec!["not-an-endpoint".into()],
        working_dir: state.path().to_path_buf(),
    };
    match Session::start(1, launch, Arc::new(Recorder::default())).await {
        Err(SessionError::ExitedEarly { code, stderr }) => {
            assert_eq!(code, Some(1));
            assert!(stderr.iter().any(|l| l.contains("Invalid endpoint")), "{stderr:?}");
        }
        other => panic!("expected ExitedEarly, got {:?}", other.map(|_| ())),
    }
}
