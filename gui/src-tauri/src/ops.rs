//! Typed operations the frontend asks for, turned into the command lines the client accepts.
//!
//! The frontend never builds a command line itself. Every argument is quoted here, in one place,
//! because the client splits on whitespace unless an argument is quoted: a GUI user will certainly
//! have a file called `My Report.pdf`. The client reads arguments with `std::quoted`, so the only
//! escapes are `\"` and `\\` - and a Windows path such as `C:\Users\me` must have its backslashes
//! escaped or they would be eaten.

use serde::Deserialize;

#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
#[serde(tag = "op", rename_all = "snake_case")]
pub enum Operation {
    List { path: String },
    Mkdir { path: String },
    Rmdir { path: String },
    /// One or more files. Several paths use the client's batch engine and report one summary.
    Delete { paths: Vec<String> },
    /// `into_directory` moves every source into `destination/` keeping its name; otherwise one
    /// source is renamed to `destination`.
    Move { sources: Vec<String>, destination: String, into_directory: bool },
    Copy { sources: Vec<String>, destination: String, into_directory: bool },
    Upload { local: String, remote: String },
    Download { remote: String, local: String },
    UploadDir { local: String, remote: String },
    DownloadDir { remote: String, local: String },
    Sync { local: String, remote: String },
    Tiers,
    SetTier { tier: String },
    VaultStatus,
    VaultInit,
    Devices,
    EnrollDevice { name: String },
    RevokeDevice { device_id: String },
}

#[derive(Debug, thiserror::Error, PartialEq, Eq)]
pub enum OpError {
    #[error("an argument contains a control character, which the client cannot receive intact")]
    ControlCharacter,
    #[error("{0} needs at least one path")]
    Empty(&'static str),
}

/// Quotes one argument for the client's `std::quoted` reader.
pub fn quote(arg: &str) -> Result<String, OpError> {
    if arg.chars().any(char::is_control) {
        return Err(OpError::ControlCharacter);
    }
    let mut out = String::with_capacity(arg.len() + 2);
    out.push('"');
    for c in arg.chars() {
        if c == '"' || c == '\\' {
            out.push('\\');
        }
        out.push(c);
    }
    out.push('"');
    Ok(out)
}

fn line(command: &str, args: &[&str]) -> Result<String, OpError> {
    let mut out = String::from(command);
    for arg in args {
        out.push(' ');
        out.push_str(&quote(arg)?);
    }
    Ok(out)
}

fn many(command: &'static str, sources: &[String], destination: &str, into_directory: bool) -> Result<String, OpError> {
    if sources.is_empty() {
        return Err(OpError::Empty(command));
    }
    // The client treats a destination with a trailing '/' as "into this directory" and expands
    // it into one full (src, dst) pair per source - see Client::batch_move_or_copy.
    let target = if into_directory && !destination.ends_with('/') {
        format!("{destination}/")
    } else {
        destination.to_owned()
    };
    let mut args: Vec<&str> = sources.iter().map(String::as_str).collect();
    args.push(&target);
    line(command, &args)
}

impl Operation {
    pub fn to_line(&self) -> Result<String, OpError> {
        use Operation::*;
        match self {
            List { path } => line("LIST", &[path]),
            Mkdir { path } => line("MKDIR", &[path]),
            Rmdir { path } => line("RMDIR", &[path]),
            Delete { paths } => {
                if paths.is_empty() {
                    return Err(OpError::Empty("DELETE"));
                }
                line("DELETE", &paths.iter().map(String::as_str).collect::<Vec<_>>())
            }
            Move { sources, destination, into_directory } => many("MOVE", sources, destination, *into_directory),
            Copy { sources, destination, into_directory } => many("COPY", sources, destination, *into_directory),
            Upload { local, remote } => line("UPLOAD", &[local, remote]),
            Download { remote, local } => line("DOWNLOAD", &[remote, local]),
            UploadDir { local, remote } => line("UPLOAD_DIR", &[local, remote]),
            DownloadDir { remote, local } => line("DOWNLOAD_DIR", &[remote, local]),
            Sync { local, remote } => line("SYNC", &[local, remote]),
            Tiers => Ok("TIERS".into()),
            SetTier { tier } => line("SET_TIER", &[tier]),
            VaultStatus => Ok("VAULT_STATUS".into()),
            VaultInit => Ok("VAULT_INIT".into()),
            Devices => Ok("DEVICES".into()),
            EnrollDevice { name } => line("ENROLL_DEVICE", &[name]),
            RevokeDevice { device_id } => line("REVOKE_DEVICE", &[device_id]),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A faithful model of `std::quoted` extraction, so the quoting is tested against what the
    /// client actually does with it rather than against itself.
    fn std_quoted_read(input: &str) -> Vec<String> {
        let mut out = Vec::new();
        let mut chars = input.chars().peekable();
        loop {
            while chars.peek().is_some_and(|c| c.is_whitespace()) {
                chars.next();
            }
            let Some(&first) = chars.peek() else { break };
            let mut token = String::new();
            if first == '"' {
                chars.next();
                while let Some(c) = chars.next() {
                    match c {
                        '\\' => {
                            if let Some(n) = chars.next() {
                                token.push(n)
                            }
                        }
                        '"' => break,
                        _ => token.push(c),
                    }
                }
            } else {
                while let Some(&c) = chars.peek() {
                    if c.is_whitespace() {
                        break;
                    }
                    token.push(c);
                    chars.next();
                }
            }
            out.push(token);
        }
        out
    }

    #[test]
    fn quoting_survives_the_clients_reader() {
        for tricky in ["plain", "My Report.pdf", r#"say "hi".txt"#, r"C:\Users\me\f.txt", r"trailing\", "", "  lead"] {
            let quoted = quote(tricky).unwrap();
            assert_eq!(std_quoted_read(&quoted), vec![tricky.to_owned()], "for {tricky:?}");
        }
    }

    #[test]
    fn control_characters_are_refused() {
        assert_eq!(quote("a\nb"), Err(OpError::ControlCharacter));
        assert_eq!(quote("a\0b"), Err(OpError::ControlCharacter));
    }

    #[test]
    fn operations_become_the_documented_command_lines() {
        let upload = Operation::Upload { local: "/home/a/My File.txt".into(), remote: "/docs/My File.txt".into() };
        assert_eq!(upload.to_line().unwrap(), r#"UPLOAD "/home/a/My File.txt" "/docs/My File.txt""#);
        assert_eq!(Operation::Tiers.to_line().unwrap(), "TIERS");
        assert_eq!(
            Operation::Delete { paths: vec!["/a".into(), "/b c".into()] }.to_line().unwrap(),
            r#"DELETE "/a" "/b c""#
        );
    }

    #[test]
    fn moving_into_a_directory_uses_a_trailing_slash() {
        let op = Operation::Move { sources: vec!["/a.txt".into(), "/b.txt".into()], destination: "/dest".into(), into_directory: true };
        assert_eq!(std_quoted_read(&op.to_line().unwrap()), vec!["MOVE", "/a.txt", "/b.txt", "/dest/"]);

        let rename = Operation::Move { sources: vec!["/a.txt".into()], destination: "/b.txt".into(), into_directory: false };
        assert_eq!(std_quoted_read(&rename.to_line().unwrap()), vec!["MOVE", "/a.txt", "/b.txt"]);
    }

    #[test]
    fn empty_batches_are_refused() {
        assert!(Operation::Delete { paths: vec![] }.to_line().is_err());
        assert!(Operation::Copy { sources: vec![], destination: "/x".into(), into_directory: true }.to_line().is_err());
    }

    #[test]
    fn operations_deserialize_from_the_frontends_shape() {
        let op: Operation = serde_json::from_str(r#"{"op":"set_tier","tier":"archive"}"#).unwrap();
        assert_eq!(op, Operation::SetTier { tier: "archive".into() });
        let op: Operation = serde_json::from_str(r#"{"op":"vault_status"}"#).unwrap();
        assert_eq!(op, Operation::VaultStatus);
    }
}
