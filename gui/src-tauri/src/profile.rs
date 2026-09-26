//! Saved servers, and how one becomes a client command line.
//!
//! A profile never contains a password - that is typed per session and handed straight to the
//! client. What it does hold is the TLS posture, because that is a property of the server, not of
//! one login: the CA file and pin a user set up once must apply to every later connection.

use std::ffi::OsString;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct Security {
    /// `--rung 3.5`: TLS 1.3 with certificate validation. Off means rung 0, plaintext.
    pub tls: bool,
    /// A private CA to validate the server against, instead of the system trust store.
    pub ca_file: Option<String>,
    /// `sha256:<hex>` of the server's public key. Checked in addition to any CA.
    pub pin: Option<String>,
    /// Name to verify the certificate against when dialling an address it does not carry.
    pub server_name: Option<String>,
    /// Refuse to connect at all without the hybrid post-quantum group.
    pub require_pq: bool,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SyncPair {
    pub local: String,
    pub remote: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Profile {
    pub id: String,
    pub name: String,
    pub host: String,
    pub port: u16,
    /// Empty means public mode: the server's shared, unauthenticated area.
    #[serde(default)]
    pub username: String,
    #[serde(default)]
    pub security: Security,
    #[serde(default)]
    pub sync_pairs: Vec<SyncPair>,
    #[serde(default)]
    pub download_dir: Option<String>,
}

fn non_empty(value: &Option<String>) -> Option<&str> {
    value.as_deref().map(str::trim).filter(|v| !v.is_empty())
}

impl Profile {
    pub fn validate(&self) -> Result<(), String> {
        if self.host.trim().is_empty() {
            return Err("A server address is required.".into());
        }
        if self.host.chars().any(|c| c.is_whitespace() || c == '@') {
            return Err("The server address cannot contain spaces or '@'.".into());
        }
        if self.port == 0 {
            return Err("The port must be between 1 and 65535.".into());
        }
        if self.username.chars().any(|c| c.is_whitespace() || c.is_control() || c == '@' || c == ':') {
            return Err("The username cannot contain spaces, '@' or ':'.".into());
        }
        if !self.security.tls && (non_empty(&self.security.ca_file).is_some() || non_empty(&self.security.pin).is_some()) {
            // The client would warn and connect in plaintext anyway; refusing here makes sure
            // nobody believes a pin protects a connection that is not even encrypted.
            return Err("A CA file or pin only applies with TLS turned on.".into());
        }
        Ok(())
    }

    /// `[user@]host:port`, the client's positional argument.
    pub fn target(&self) -> String {
        let host = self.host.trim();
        if self.username.is_empty() {
            format!("{host}:{}", self.port)
        } else {
            format!("{}@{host}:{}", self.username, self.port)
        }
    }

    /// Same key the client files device keys under (`Client::account_key`).
    pub fn account_key(&self) -> String {
        let user = if self.username.is_empty() { "public" } else { &self.username };
        format!("{user}@{}:{}", self.host.trim(), self.port)
    }

    /// Directory name for this account's client state. One per account is what stops two
    /// sessions from losing each other's resume state (they share a partmeta.json otherwise).
    pub fn state_dir_name(&self) -> String {
        self.account_key()
            .chars()
            .map(|c| if c.is_ascii_alphanumeric() || matches!(c, '.' | '-' | '_' | '@') { c } else { '_' })
            .collect()
    }

    /// Every argument but `--ipc`, which the session adds.
    pub fn client_args(&self, state_dir: &Path) -> Vec<OsString> {
        let mut args: Vec<OsString> = vec![self.target().into()];
        args.push("--state-dir".into());
        args.push(state_dir.as_os_str().to_owned());
        args.push("--log".into());
        args.push(state_dir.join("client.log").into_os_string());

        if self.security.tls {
            args.push("--rung".into());
            args.push("3.5".into());
            if let Some(ca) = non_empty(&self.security.ca_file) {
                args.push("--ca-file".into());
                args.push(ca.into());
            }
            if let Some(pin) = non_empty(&self.security.pin) {
                args.push("--pin".into());
                args.push(pin.into());
            }
            if let Some(name) = non_empty(&self.security.server_name) {
                args.push("--tls-servername".into());
                args.push(name.into());
            }
            if self.security.require_pq {
                args.push("--tls-require-pq".into());
            }
        }
        args
    }
}

/// Where the bundled client lives: beside this executable, where Tauri's `externalBin` puts it
/// (with the target triple stripped from its name). Debug builds may point elsewhere with
/// `MINIDRIVE_CLIENT_BIN`, which is how the end-to-end tests run a freshly built client.
pub fn client_binary() -> Result<PathBuf, String> {
    #[cfg(debug_assertions)]
    if let Some(path) = std::env::var_os("MINIDRIVE_CLIENT_BIN") {
        return Ok(PathBuf::from(path));
    }
    let exe = std::env::current_exe().map_err(|e| format!("cannot locate this program: {e}"))?;
    let dir = exe.parent().ok_or("this program has no parent directory")?;
    let path = dir.join(format!("minidrive-client{}", std::env::consts::EXE_SUFFIX));
    if path.is_file() {
        Ok(path)
    } else {
        Err(format!("The bundled MiniDrive client is missing: {}", path.display()))
    }
}

/// Creates a directory only this user can read (0700 on Unix; Windows profile directories are
/// already per-user).
pub fn create_private_dir(path: &Path) -> std::io::Result<()> {
    std::fs::create_dir_all(path)?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o700))?;
    }
    Ok(())
}

#[derive(Debug, Default, Serialize, Deserialize)]
struct ProfileFile {
    version: u32,
    profiles: Vec<Profile>,
}

pub fn load_profiles(path: &Path) -> Result<Vec<Profile>, String> {
    match std::fs::read(path) {
        Ok(bytes) => serde_json::from_slice::<ProfileFile>(&bytes)
            .map(|f| f.profiles)
            .map_err(|e| format!("{} is unreadable: {e}", path.display())),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(Vec::new()),
        Err(e) => Err(format!("cannot read {}: {e}", path.display())),
    }
}

/// Write-then-rename, so a crash mid-save leaves the previous list rather than half of one.
pub fn save_profiles(path: &Path, profiles: &[Profile]) -> Result<(), String> {
    for profile in profiles {
        profile.validate().map_err(|e| format!("{}: {e}", profile.name))?;
    }
    let dir = path.parent().ok_or("profiles file has no directory")?;
    create_private_dir(dir).map_err(|e| format!("cannot create {}: {e}", dir.display()))?;
    let body = serde_json::to_vec_pretty(&ProfileFile { version: 1, profiles: profiles.to_vec() }).map_err(|e| e.to_string())?;
    let temp = dir.join(format!(".profiles-{}.tmp", crate::endpoint::random_hex(6)));
    std::fs::write(&temp, body).map_err(|e| format!("cannot write {}: {e}", temp.display()))?;
    std::fs::rename(&temp, path).map_err(|e| {
        let _ = std::fs::remove_file(&temp);
        format!("cannot replace {}: {e}", path.display())
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn profile() -> Profile {
        Profile {
            id: "1".into(),
            name: "Home NAS".into(),
            host: "nas.local".into(),
            port: 9000,
            username: "alice".into(),
            security: Security::default(),
            sync_pairs: vec![],
            download_dir: None,
        }
    }

    #[test]
    fn target_and_account_key_match_the_clients() {
        let mut p = profile();
        assert_eq!(p.target(), "alice@nas.local:9000");
        assert_eq!(p.account_key(), "alice@nas.local:9000");
        p.username.clear();
        assert_eq!(p.target(), "nas.local:9000");
        assert_eq!(p.account_key(), "public@nas.local:9000");
        assert_eq!(p.state_dir_name(), "public@nas.local_9000");
    }

    #[test]
    fn plaintext_profiles_pass_no_tls_flags() {
        let args = profile().client_args(Path::new("/state"));
        let args: Vec<String> = args.into_iter().map(|a| a.into_string().unwrap()).collect();
        assert_eq!(args[0], "alice@nas.local:9000");
        assert!(args.windows(2).any(|w| w == ["--state-dir", "/state"]));
        assert!(!args.iter().any(|a| a.starts_with("--rung") || a.starts_with("--tls") || a == "--pin"));
    }

    #[test]
    fn tls_profiles_pass_their_whole_posture() {
        let mut p = profile();
        p.security = Security {
            tls: true,
            ca_file: Some("/etc/md/ca.crt".into()),
            pin: Some("sha256:ab".into()),
            server_name: Some("  ".into()), // blank is ignored, not passed as an empty name
            require_pq: true,
        };
        let args: Vec<String> = p.client_args(Path::new("/s")).into_iter().map(|a| a.into_string().unwrap()).collect();
        for pair in [["--rung", "3.5"], ["--ca-file", "/etc/md/ca.crt"], ["--pin", "sha256:ab"]] {
            assert!(args.windows(2).any(|w| w == pair), "missing {pair:?} in {args:?}");
        }
        assert!(args.contains(&"--tls-require-pq".to_owned()));
        assert!(!args.contains(&"--tls-servername".to_owned()));
        assert!(!args.iter().any(|a| a == "none"), "the GUI never turns verification off");
    }

    #[test]
    fn a_pin_without_tls_is_refused_rather_than_silently_ignored() {
        let mut p = profile();
        p.security.pin = Some("sha256:ab".into());
        assert!(p.validate().is_err());
        p.security.tls = true;
        assert!(p.validate().is_ok());
    }

    #[test]
    fn profiles_round_trip_through_disk() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("sub").join("profiles.json");
        assert_eq!(load_profiles(&path).unwrap(), vec![]);
        let mut p = profile();
        p.sync_pairs.push(SyncPair { local: "/home/a/Docs".into(), remote: "/Docs".into() });
        save_profiles(&path, &[p.clone()]).unwrap();
        assert_eq!(load_profiles(&path).unwrap(), vec![p]);
    }
}
