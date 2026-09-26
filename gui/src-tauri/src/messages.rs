//! What the client sends back over the IPC channel, parsed into types the rest of the core (and,
//! serialized again, the frontend) can match on. The authoritative description is `docs/ipc.md`.

use serde::{Deserialize, Serialize};
use serde_json::Value;

/// What the client is waiting for. Exactly one PROMPT ends every exchange, which is what lets the
/// core know a command is finished - there is no other "done" signal in the protocol.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum PromptKind {
    Command,
    Password,
    Confirm,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct Prompt {
    pub kind: PromptKind,
    /// The password prompt's label, or the question a confirm answers. Empty for commands.
    pub text: String,
}

/// Everything that is not a PROMPT, in the order the client sent it.
#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(tag = "type", rename_all = "lowercase")]
pub enum Frame {
    /// The outcome of a command (the CLI's `OK: ...` / `ERROR: <code> ...` line). `code` is the
    /// protocol's HTTP-shaped status code.
    Result { ok: bool, code: i64, message: String },
    /// Free-form output the CLI would print - kept for the activity log.
    Info { text: String },
    /// A push notification: `transfer_progress`, `listing`, `tiers`, `devices`, `vault_status`,
    /// `batch_item`, `batch_summary`. `data` is the whole frame, so new fields reach the frontend
    /// without a change here.
    Event { name: String, data: Value },
}

#[derive(Debug, Clone, PartialEq)]
pub enum Incoming {
    Frame(Frame),
    Prompt(Prompt),
}

/// Parses one frame body. Unknown frame types are skipped rather than treated as errors, so a newer
/// client can add message kinds without breaking an older GUI.
pub fn parse(value: Value) -> Option<Incoming> {
    let kind = value.get("type")?.as_str()?.to_owned();
    let text = |key: &str| value.get(key).and_then(Value::as_str).unwrap_or_default().to_owned();
    match kind.as_str() {
        "RESULT" => Some(Incoming::Frame(Frame::Result {
            ok: value.get("ok").and_then(Value::as_bool).unwrap_or(false),
            code: value.get("code").and_then(Value::as_i64).unwrap_or(0),
            message: text("message"),
        })),
        "INFO" => Some(Incoming::Frame(Frame::Info { text: text("text") })),
        "PROMPT" => {
            let prompt_kind = match value.get("kind").and_then(Value::as_str) {
                Some("password") => PromptKind::Password,
                Some("confirm") => PromptKind::Confirm,
                _ => PromptKind::Command,
            };
            Some(Incoming::Prompt(Prompt { kind: prompt_kind, text: text("text") }))
        }
        "EVENT" => {
            let name = text("event");
            Some(Incoming::Frame(Frame::Event { name, data: value }))
        }
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn parses_every_documented_frame() {
        assert_eq!(
            parse(json!({"type":"RESULT","ok":false,"code":404,"message":"File does not exist."})),
            Some(Incoming::Frame(Frame::Result { ok: false, code: 404, message: "File does not exist.".into() }))
        );
        assert_eq!(
            parse(json!({"type":"PROMPT","kind":"password","text":"Password for alice: "})),
            Some(Incoming::Prompt(Prompt { kind: PromptKind::Password, text: "Password for alice: ".into() }))
        );
        assert_eq!(
            parse(json!({"type":"PROMPT","kind":"confirm","text":"Register?"})),
            Some(Incoming::Prompt(Prompt { kind: PromptKind::Confirm, text: "Register?".into() }))
        );
        match parse(json!({"type":"EVENT","event":"listing","path":"/","entries":[]})) {
            Some(Incoming::Frame(Frame::Event { name, data })) => {
                assert_eq!(name, "listing");
                assert_eq!(data["path"], "/");
            }
            other => panic!("unexpected {other:?}"),
        }
    }

    #[test]
    fn unknown_frames_are_skipped_not_fatal() {
        assert_eq!(parse(json!({"type":"SOMETHING_NEW"})), None);
        assert_eq!(parse(json!({"no":"type"})), None);
        assert_eq!(parse(json!([1, 2, 3])), None);
    }

    #[test]
    fn frames_serialize_for_the_frontend_with_a_type_tag() {
        let frame = Frame::Result { ok: true, code: 200, message: "ok".into() };
        assert_eq!(serde_json::to_value(&frame).unwrap(), json!({"type":"result","ok":true,"code":200,"message":"ok"}));
    }
}
