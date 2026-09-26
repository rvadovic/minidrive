//! The Tauri layer: commands the frontend invokes, and events it listens to.
//!
//! Commands (`invoke`): `connect`, `cancel_connect`, `execute`, `respond`, `disconnect`,
//! `load_profiles`, `save_profiles`, `inspect_local_paths`, `default_download_dir`.
//! Events (`listen`): `minidrive://frame` for every frame as it arrives (progress bars, the
//! activity log), `minidrive://ended` when a session is over.
//!
//! Nothing here speaks the protocol or touches a key. It starts the bundled client, hands it lines,
//! and passes back what it says.

use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;

use serde::Serialize;
use tauri::{AppHandle, Emitter, Manager, State};
use tokio::sync::{Mutex, Notify};
use zeroize::Zeroize;

use crate::messages::Frame;
use crate::ops::Operation;
use crate::profile::{self, Profile};
use crate::session::{Ending, Exchange, Launch, Observer, Session};

#[derive(Default)]
pub struct AppState {
    session: Mutex<Option<Arc<Session>>>,
    next_id: AtomicU64,
    connecting: AtomicBool,
    cancel_connect: Notify,
}

#[derive(Clone, Serialize)]
struct FrameEvent<'a> {
    session: u64,
    frame: &'a Frame,
}

#[derive(Clone, Serialize)]
struct EndedEvent<'a> {
    session: u64,
    ending: &'a Ending,
}

struct TauriObserver {
    app: AppHandle,
}

impl Observer for TauriObserver {
    fn frame(&self, session: u64, frame: &Frame) {
        let _ = self.app.emit("minidrive://frame", FrameEvent { session, frame });
    }

    fn ended(&self, session: u64, ending: &Ending) {
        let _ = self.app.emit("minidrive://ended", EndedEvent { session, ending });
        let app = self.app.clone();
        tauri::async_runtime::spawn(async move {
            let state = app.state::<AppState>();
            let mut current = state.session.lock().await;
            if current.as_ref().map(|s| s.id()) == Some(session) {
                *current = None;
            }
        });
    }
}

#[derive(Serialize)]
pub struct Connected {
    session: u64,
    exchange: Exchange,
}

fn path_of(app: &AppHandle, which: &str) -> Result<PathBuf, String> {
    let paths = app.path();
    match which {
        "config" => paths.app_config_dir(),
        _ => paths.app_data_dir(),
    }
    .map_err(|e| format!("cannot resolve the {which} directory: {e}"))
}

async fn current(state: &AppState) -> Result<Arc<Session>, String> {
    state.session.lock().await.clone().ok_or_else(|| "Not connected.".to_owned())
}

#[tauri::command]
pub async fn connect(app: AppHandle, state: State<'_, AppState>, profile: Profile) -> Result<Connected, String> {
    profile.validate()?;
    if state.session.lock().await.is_some() {
        return Err("Already connected. Disconnect first.".into());
    }
    if state.connecting.swap(true, Ordering::SeqCst) {
        return Err("A connection is already being made.".into());
    }
    let result = connect_inner(&app, &state, profile).await;
    state.connecting.store(false, Ordering::SeqCst);
    result
}

async fn connect_inner(app: &AppHandle, state: &AppState, profile: Profile) -> Result<Connected, String> {
    let state_dir = path_of(app, "data")?.join("accounts").join(profile.state_dir_name());
    profile::create_private_dir(&state_dir).map_err(|e| format!("cannot create {}: {e}", state_dir.display()))?;

    let launch = Launch {
        program: profile::client_binary()?,
        args: profile.client_args(&state_dir),
        working_dir: state_dir,
    };
    let id = state.next_id.fetch_add(1, Ordering::Relaxed) + 1;
    let observer = Arc::new(TauriObserver { app: app.clone() });
    // Connecting can hang for as long as the OS's TCP timeout when a host drops packets, so it can
    // be cancelled. Dropping the start future kills a client that has not connected yet, and closes
    // the channel of one that has (see Session::start).
    let (session, exchange) = tokio::select! {
        started = Session::start(id, launch, observer) => started.map_err(|e| e.to_string())?,
        _ = state.cancel_connect.notified() => return Err("Cancelled.".into()),
    };
    if exchange.ended() {
        // Never reached a prompt: refused certificate, unreachable server. The reason is on the
        // client's stderr or in its last error, not in anything the frontend could act on.
        return Err(describe_failure(&session.wait_finished().await));
    }
    *state.session.lock().await = Some(session);
    Ok(Connected { session: id, exchange })
}

#[tauri::command]
pub fn cancel_connect(state: State<'_, AppState>) {
    // notify_waiters, not notify_one: a cancel that arrives after the connect finished must not
    // linger as a stored permit and cancel the next one.
    if state.connecting.load(Ordering::SeqCst) {
        state.cancel_connect.notify_waiters();
    }
}

fn describe_failure(ending: &Ending) -> String {
    let mut lines: Vec<&str> = ending.stderr.iter().map(String::as_str).collect();
    if let Some(error) = &ending.last_error {
        if !lines.iter().any(|l| l.contains(error.as_str())) {
            lines.push(error);
        }
    }
    if lines.is_empty() {
        format!("The connection closed before logging in (client exit code {:?}).", ending.exit_code)
    } else {
        lines.join("\n")
    }
}

#[tauri::command]
pub async fn execute(state: State<'_, AppState>, op: Operation) -> Result<Exchange, String> {
    let line = op.to_line().map_err(|e| e.to_string())?;
    let session = current(&state).await?; // lock released: disconnect must stay possible meanwhile
    session.command(&line).await.map_err(|e| e.to_string())
}

#[tauri::command]
pub async fn respond(state: State<'_, AppState>, mut answer: String) -> Result<Exchange, String> {
    let session = current(&state).await;
    let result = match session {
        Ok(session) => session.respond(&answer).await.map_err(|e| e.to_string()),
        Err(e) => Err(e),
    };
    answer.zeroize(); // may be the password
    result
}

#[tauri::command]
pub async fn disconnect(state: State<'_, AppState>) -> Result<Option<Ending>, String> {
    let session = state.session.lock().await.take();
    match session {
        Some(session) => Ok(Some(session.close().await)),
        None => Ok(None),
    }
}

#[tauri::command]
pub fn load_profiles(app: AppHandle) -> Result<Vec<Profile>, String> {
    profile::load_profiles(&path_of(&app, "config")?.join("profiles.json"))
}

#[tauri::command]
pub fn save_profiles(app: AppHandle, profiles: Vec<Profile>) -> Result<(), String> {
    profile::save_profiles(&path_of(&app, "config")?.join("profiles.json"), &profiles)
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct LocalPath {
    path: String,
    name: String,
    exists: bool,
    is_directory: bool,
    size: u64,
}

/// What dropped or picked paths are, so the frontend can choose UPLOAD or UPLOAD_DIR.
#[tauri::command]
pub fn inspect_local_paths(paths: Vec<String>) -> Vec<LocalPath> {
    paths
        .into_iter()
        .map(|path| {
            let meta = std::fs::metadata(&path).ok();
            let name = std::path::Path::new(&path).file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
            LocalPath {
                exists: meta.is_some(),
                is_directory: meta.as_ref().is_some_and(|m| m.is_dir()),
                size: meta.as_ref().map_or(0, |m| m.len()),
                name,
                path,
            }
        })
        .collect()
}

#[tauri::command]
pub fn default_download_dir(app: AppHandle) -> Option<String> {
    app.path().download_dir().ok().map(|p| p.to_string_lossy().into_owned())
}

/// Closes the session when the window goes away, so the client saves transfer state and exits
/// rather than being killed.
pub fn shutdown(app: &AppHandle) {
    let state = app.state::<AppState>();
    let session = tauri::async_runtime::block_on(async { state.session.lock().await.take() });
    if let Some(session) = session {
        tauri::async_runtime::block_on(session.close());
    }
}
