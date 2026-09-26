//! The IPC channel's framing: a 4-byte big-endian length, then a JSON body.
//!
//! This is deliberately the framing the client already speaks to the server
//! (`shared/src/transport/stream.cpp`), so the C++ side has one framing, not two. See
//! `docs/ipc.md` for the messages that ride on it.

use serde::Serialize;
use serde_json::Value;
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};
use zeroize::Zeroize;

/// `transport::MAX_MESSAGE_SIZE` on the C++ side. The client refuses anything larger, and so do we:
/// four bytes the peer chooses must not be able to ask for an arbitrary allocation.
pub const MAX_FRAME: u32 = 64 * 1024 * 1024;

#[derive(Debug, thiserror::Error)]
pub enum FrameError {
    #[error("the client closed the channel")]
    Closed,
    #[error("frame length {0} is out of range")]
    Length(usize),
    #[error("frame is not valid JSON: {0}")]
    Json(#[from] serde_json::Error),
    #[error("IPC channel error: {0}")]
    Io(std::io::Error),
}

impl From<std::io::Error> for FrameError {
    fn from(e: std::io::Error) -> Self {
        if e.kind() == std::io::ErrorKind::UnexpectedEof {
            FrameError::Closed
        } else {
            FrameError::Io(e)
        }
    }
}

pub async fn read_frame<R: AsyncRead + Unpin>(reader: &mut R) -> Result<Value, FrameError> {
    let mut header = [0u8; 4];
    reader.read_exact(&mut header).await?;
    let len = u32::from_be_bytes(header);
    if len == 0 || len > MAX_FRAME {
        return Err(FrameError::Length(len as usize));
    }
    let mut body = vec![0u8; len as usize];
    reader.read_exact(&mut body).await?;
    Ok(serde_json::from_slice(&body)?)
}

#[derive(Serialize)]
struct CommandFrame<'a> {
    #[serde(rename = "type")]
    kind: &'static str,
    line: &'a str,
}

/// Sends one COMMAND frame. The line may be a password, so it is serialized straight from the
/// borrowed string into one buffer, and that buffer is wiped once written - no intermediate
/// `serde_json::Value` holds an unwipeable copy.
pub async fn write_command<W: AsyncWrite + Unpin>(writer: &mut W, line: &str) -> Result<(), FrameError> {
    let mut body = serde_json::to_vec(&CommandFrame { kind: "COMMAND", line })?;
    if body.len() > MAX_FRAME as usize {
        let len = body.len();
        body.zeroize();
        return Err(FrameError::Length(len));
    }
    let mut frame = Vec::with_capacity(4 + body.len());
    frame.extend_from_slice(&(body.len() as u32).to_be_bytes());
    frame.extend_from_slice(&body);
    body.zeroize();

    let result = async {
        writer.write_all(&frame).await?;
        writer.flush().await
    }
    .await;
    frame.zeroize();
    result.map_err(FrameError::from)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn command_frames_round_trip_with_big_endian_length() {
        let (mut a, mut b) = tokio::io::duplex(1024);
        write_command(&mut a, "UPLOAD \"a b\" c").await.unwrap();

        let mut header = [0u8; 4];
        b.read_exact(&mut header).await.unwrap();
        let len = u32::from_be_bytes(header) as usize;
        let mut body = vec![0u8; len];
        b.read_exact(&mut body).await.unwrap();
        let value: Value = serde_json::from_slice(&body).unwrap();
        assert_eq!(value["type"], "COMMAND");
        assert_eq!(value["line"], "UPLOAD \"a b\" c");
    }

    #[tokio::test]
    async fn reads_a_frame_and_reports_a_clean_close() {
        let (mut a, mut b) = tokio::io::duplex(1024);
        let body = br#"{"type":"INFO","text":"hi"}"#;
        a.write_all(&(body.len() as u32).to_be_bytes()).await.unwrap();
        a.write_all(body).await.unwrap();
        drop(a);

        let value = read_frame(&mut b).await.unwrap();
        assert_eq!(value["text"], "hi");
        assert!(matches!(read_frame(&mut b).await, Err(FrameError::Closed)));
    }

    #[tokio::test]
    async fn refuses_out_of_range_lengths_without_allocating() {
        let (mut a, mut b) = tokio::io::duplex(64);
        a.write_all(&u32::MAX.to_be_bytes()).await.unwrap();
        assert!(matches!(read_frame(&mut b).await, Err(FrameError::Length(_))));

        let (mut a, mut b) = tokio::io::duplex(64);
        a.write_all(&0u32.to_be_bytes()).await.unwrap();
        assert!(matches!(read_frame(&mut b).await, Err(FrameError::Length(0))));
    }

    #[tokio::test]
    async fn a_truncated_body_is_a_close_not_a_parse_error() {
        let (mut a, mut b) = tokio::io::duplex(64);
        a.write_all(&10u32.to_be_bytes()).await.unwrap();
        a.write_all(b"{\"ty").await.unwrap();
        drop(a);
        assert!(matches!(read_frame(&mut b).await, Err(FrameError::Closed)));
    }
}
