//! MiniDrive's desktop GUI core.
//!
//! The GUI implements no crypto and no protocol logic: it drives the bundled C++ client in its
//! headless `--ipc` mode, which already has the wire protocol, TLS with pinning, the SYNC and batch
//! engines, resume, and the vault's whole key hierarchy. See `docs/gui.md`.

pub mod app;
pub mod endpoint;
pub mod framing;
pub mod messages;
pub mod ops;
pub mod profile;
pub mod session;

pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .manage(app::AppState::default())
        .invoke_handler(tauri::generate_handler![
            app::connect,
            app::cancel_connect,
            app::execute,
            app::respond,
            app::disconnect,
            app::load_profiles,
            app::save_profiles,
            app::inspect_local_paths,
            app::default_download_dir,
        ])
        .build(tauri::generate_context!())
        .expect("error while building the MiniDrive application")
        .run(|handle, event| {
            if let tauri::RunEvent::Exit = event {
                app::shutdown(handle);
            }
        });
}
