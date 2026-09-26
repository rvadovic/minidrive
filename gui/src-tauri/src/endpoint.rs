//! The host end of the IPC channel.
//!
//! The host creates the endpoint and the client connects to it (`docs/ipc.md`, "Who owns the
//! endpoint"). Anyone who can connect can drive the client - and a vaulted session holds the vault
//! key in memory - so two things are enforced here, not left to convention:
//!
//! - **Reachability.** On Unix the socket lives in a directory created fresh with mode 0700; the
//!   create fails if the name already exists, so nobody can pre-plant it. On Windows the pipe name
//!   carries 128 random bits and is created as the first and only instance, local clients only.
//! - **Identity.** After accepting, the peer's process id must be the child this session spawned.
//!   A squatter that raced the real client in would be refused, and the session fails to start
//!   rather than being handed to it.

use std::ffi::OsString;
use std::io;

pub fn random_hex(bytes: usize) -> String {
    let mut buf = vec![0u8; bytes];
    getrandom::fill(&mut buf).expect("the operating system's random source is unavailable");
    buf.iter().map(|b| format!("{b:02x}")).collect()
}

fn denied(expected: u32, actual: Option<u32>) -> io::Error {
    io::Error::new(
        io::ErrorKind::PermissionDenied,
        format!(
            "the IPC peer is not the client this session started (expected pid {expected}, got {})",
            actual.map_or_else(|| "unknown".to_owned(), |p| p.to_string())
        ),
    )
}

#[cfg(unix)]
mod imp {
    use super::*;
    use std::os::unix::fs::DirBuilderExt;
    use std::path::PathBuf;
    use tokio::net::{UnixListener, UnixStream};

    pub type IpcStream = UnixStream;

    pub struct Endpoint {
        listener: UnixListener,
        dir: PathBuf,
        path: PathBuf,
    }

    // sun_path is 108 bytes on Linux and 104 on macOS, whose per-user temp directory is already
    // long. Leave room for the terminating NUL.
    const MAX_SOCKET_PATH: usize = 100;

    impl Endpoint {
        pub fn create() -> io::Result<Self> {
            let runtime = std::env::var_os("XDG_RUNTIME_DIR").map(PathBuf::from).filter(|p| p.is_dir());
            let mut bases: Vec<PathBuf> = runtime.into_iter().collect();
            bases.push(std::env::temp_dir());
            bases.push(PathBuf::from("/tmp"));

            let mut last_error = io::Error::other("no usable directory for the IPC socket");
            for base in bases {
                let dir = base.join(format!("minidrive-{}", random_hex(8)));
                let path = dir.join("ipc.sock");
                if path.as_os_str().len() > MAX_SOCKET_PATH {
                    continue;
                }
                // create(), not create_all(): an existing directory is an error, so the 0700 mode
                // is guaranteed to be ours rather than whatever someone left there.
                if let Err(e) = std::fs::DirBuilder::new().mode(0o700).create(&dir) {
                    last_error = e;
                    continue;
                }
                match UnixListener::bind(&path) {
                    Ok(listener) => return Ok(Endpoint { listener, dir, path }),
                    Err(e) => {
                        let _ = std::fs::remove_dir(&dir);
                        last_error = e;
                    }
                }
            }
            Err(last_error)
        }

        /// What to pass as `--ipc`.
        pub fn argument(&self) -> OsString {
            self.path.clone().into_os_string()
        }

        pub async fn accept(&self, expected_pid: u32) -> io::Result<IpcStream> {
            let (stream, _) = self.listener.accept().await?;
            let pid = stream.peer_cred()?.pid().and_then(|p| u32::try_from(p).ok());
            if pid != Some(expected_pid) {
                return Err(denied(expected_pid, pid));
            }
            Ok(stream)
        }
    }

    impl Drop for Endpoint {
        fn drop(&mut self) {
            let _ = std::fs::remove_file(&self.path);
            let _ = std::fs::remove_dir(&self.dir);
        }
    }
}

#[cfg(windows)]
mod imp {
    use super::*;
    use std::os::windows::io::AsRawHandle;
    use tokio::net::windows::named_pipe::{NamedPipeServer, ServerOptions};
    use tokio::sync::Mutex;

    pub type IpcStream = NamedPipeServer;

    pub struct Endpoint {
        name: String,
        server: Mutex<Option<NamedPipeServer>>,
    }

    impl Endpoint {
        pub fn create() -> io::Result<Self> {
            let name = format!(r"\\.\pipe\minidrive-{}", random_hex(16));
            let server = ServerOptions::new()
                .first_pipe_instance(true) // fail rather than join a pipe someone else created
                .max_instances(1)
                .reject_remote_clients(true)
                .create(&name)?;
            Ok(Endpoint { name, server: Mutex::new(Some(server)) })
        }

        pub fn argument(&self) -> OsString {
            OsString::from(&self.name)
        }

        pub async fn accept(&self, expected_pid: u32) -> io::Result<IpcStream> {
            let server = self
                .server
                .lock()
                .await
                .take()
                .ok_or_else(|| io::Error::other("the IPC pipe was already accepted"))?;
            server.connect().await?;

            let mut pid: u32 = 0;
            // SAFETY: the handle is a live named pipe owned by `server` for the whole call.
            let ok = unsafe {
                windows_sys::Win32::System::Pipes::GetNamedPipeClientProcessId(
                    server.as_raw_handle() as windows_sys::Win32::Foundation::HANDLE,
                    &mut pid,
                )
            };
            if ok == 0 {
                return Err(io::Error::last_os_error());
            }
            if pid != expected_pid {
                return Err(denied(expected_pid, Some(pid)));
            }
            Ok(server)
        }
    }
}

pub use imp::{Endpoint, IpcStream};

#[cfg(all(test, unix))]
mod tests {
    use super::*;
    use std::os::unix::fs::PermissionsExt;
    use std::path::Path;

    #[tokio::test]
    async fn socket_directory_is_private_and_removed_on_drop() {
        let endpoint = Endpoint::create().unwrap();
        let path = std::path::PathBuf::from(endpoint.argument());
        let dir = path.parent().unwrap().to_path_buf();
        let mode = std::fs::metadata(&dir).unwrap().permissions().mode() & 0o777;
        assert_eq!(mode, 0o700, "socket directory must be owner-only");
        assert!(path.exists());
        drop(endpoint);
        assert!(!Path::new(&dir).exists());
    }

    #[tokio::test]
    async fn a_peer_that_is_not_the_spawned_child_is_refused() {
        let endpoint = Endpoint::create().unwrap();
        let path = std::path::PathBuf::from(endpoint.argument());
        // This test process connects, but the endpoint expects some other pid.
        let connect = tokio::spawn(async move { tokio::net::UnixStream::connect(path).await });
        let not_us = std::process::id().wrapping_add(1);
        let err = endpoint.accept(not_us).await.unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::PermissionDenied);
        let _ = connect.await;
    }

    #[tokio::test]
    async fn the_spawned_child_is_accepted() {
        let endpoint = Endpoint::create().unwrap();
        let path = std::path::PathBuf::from(endpoint.argument());
        let connect = tokio::spawn(async move { tokio::net::UnixStream::connect(path).await });
        endpoint.accept(std::process::id()).await.unwrap();
        connect.await.unwrap().unwrap();
    }
}
