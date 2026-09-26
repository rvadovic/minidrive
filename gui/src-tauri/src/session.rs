//! One running client and the conversation with it.
//!
//! The client is strictly conversational: it takes one line, works, and then emits exactly one
//! PROMPT saying what it wants next. So an exchange is "send one line, collect every frame until
//! the next PROMPT", and a PROMPT is the only completion signal there is. Two rules follow, and
//! this module is where they are enforced:
//!
//! - **One exchange at a time.** The conversation sits behind a mutex held for the whole exchange,
//!   so a second command waits for the first to finish instead of being queued inside the client
//!   where it could be read as the answer to a question.
//! - **The right kind of line for the prompt.** A command is only sent while the client is at a
//!   command prompt, and an answer only while it is asking a question or for the password. A LIST
//!   fired off by a refresh timer can never be taken as the reply to "Migrate your data?".
//!
//! Frames are also handed to an [`Observer`] the moment they arrive, so a progress bar moves while
//! a long exchange is still running.

use std::collections::VecDeque;
use std::ffi::OsString;
use std::path::PathBuf;
use std::process::Stdio;
use std::sync::{Arc, Mutex as StdMutex};
use std::time::Duration;

use serde::Serialize;
use tokio::io::{AsyncBufReadExt, AsyncRead, AsyncWriteExt, BufReader, WriteHalf};
use tokio::process::{Child, Command};
use tokio::sync::{mpsc, watch, Mutex, Notify};

use crate::endpoint::{Endpoint, IpcStream};
use crate::framing::{read_frame, write_command, FrameError};
use crate::messages::{parse, Frame, Incoming, Prompt, PromptKind};

/// How long the client gets to connect to the socket after it is started. Connecting happens
/// before the client does any network I/O, so this bounds process startup, not the server.
const ACCEPT_TIMEOUT: Duration = Duration::from_secs(15);
/// How long a client that has lost its channel gets to save transfer state and exit by itself.
const EXIT_GRACE: Duration = Duration::from_secs(10);
const STDERR_LINES: usize = 40;

/// How to start the client. `args` is everything except `--ipc`, which the session adds.
#[derive(Debug, Clone)]
pub struct Launch {
    pub program: PathBuf,
    pub args: Vec<OsString>,
    pub working_dir: PathBuf,
}

/// Receives frames as they arrive and the end of the session. Implemented by the Tauri layer
/// (which forwards to the webview) and by tests.
pub trait Observer: Send + Sync + 'static {
    fn frame(&self, session: u64, frame: &Frame);
    fn ended(&self, session: u64, ending: &Ending);
}

/// The result of one exchange: every frame the client sent, and what it is waiting for now.
/// `prompt` is `None` exactly when the session ended during the exchange.
#[derive(Debug, Clone, Serialize)]
pub struct Exchange {
    pub frames: Vec<Frame>,
    pub prompt: Option<Prompt>,
}

impl Exchange {
    pub fn ended(&self) -> bool {
        self.prompt.is_none()
    }

    /// The last RESULT frame, which is the outcome of a simple command.
    pub fn last_result(&self) -> Option<(bool, i64, &str)> {
        self.frames.iter().rev().find_map(|f| match f {
            Frame::Result { ok, code, message } => Some((*ok, *code, message.as_str())),
            _ => None,
        })
    }
}

/// Why and how a session ended - enough to tell "wrong password, locked out" from "TLS refused
/// the certificate" from "the server went away".
#[derive(Debug, Clone, Serialize)]
pub struct Ending {
    pub exit_code: Option<i32>,
    /// The client's last stderr lines. TLS failures and startup errors are reported there, not
    /// over the channel, because they happen before (or instead of) a working channel.
    pub stderr: Vec<String>,
    /// The last error RESULT the client sent, if any.
    pub last_error: Option<String>,
}

#[derive(Debug, thiserror::Error)]
pub enum SessionError {
    #[error("could not create the IPC endpoint: {0}")]
    Endpoint(std::io::Error),
    #[error("could not start the MiniDrive client ({program}): {source}")]
    Spawn { program: String, source: std::io::Error },
    #[error("the MiniDrive client exited before connecting (exit code {code:?}){}", stderr_suffix(.stderr))]
    ExitedEarly { code: Option<i32>, stderr: Vec<String> },
    #[error("the MiniDrive client did not connect within {0:?}")]
    AcceptTimeout(Duration),
    #[error("IPC connection refused: {0}")]
    Accept(std::io::Error),
    #[error("the session has ended")]
    Ended,
    #[error("the client is waiting for {expected:?}, not {offered}")]
    WrongPrompt { expected: PromptKind, offered: &'static str },
    #[error(transparent)]
    Frame(#[from] FrameError),
}

fn stderr_suffix(lines: &[String]) -> String {
    if lines.is_empty() {
        String::new()
    } else {
        format!(": {}", lines.join(" / "))
    }
}

struct Conversation {
    incoming: mpsc::UnboundedReceiver<Incoming>,
    prompt: Option<Prompt>,
}

pub struct Session {
    id: u64,
    writer: Mutex<Option<WriteHalf<IpcStream>>>,
    // Tells the reader to let go of its half. The channel only really closes once both halves
    // are dropped: shutting down the write half delivers EOF on a Unix socket, but is only a flush
    // on a Windows named pipe.
    stop: Arc<Notify>,
    conversation: Mutex<Conversation>,
    finished: watch::Receiver<Option<Ending>>,
}

type StderrTail = Arc<StdMutex<VecDeque<String>>>;

struct StopOnDrop(Option<Arc<Notify>>);

impl Drop for StopOnDrop {
    fn drop(&mut self) {
        if let Some(stop) = self.0.take() {
            stop.notify_one();
        }
    }
}

fn drain_lines<R: AsyncRead + Unpin + Send + 'static>(reader: R, keep: Option<StderrTail>) {
    tokio::spawn(async move {
        let mut lines = BufReader::new(reader).lines();
        while let Ok(Some(line)) = lines.next_line().await {
            if let Some(tail) = &keep {
                let mut tail = tail.lock().unwrap();
                if tail.len() == STDERR_LINES {
                    tail.pop_front();
                }
                tail.push_back(line);
            }
        }
    });
}

fn snapshot(tail: &StderrTail) -> Vec<String> {
    tail.lock().unwrap().iter().filter(|l| !l.trim().is_empty()).cloned().collect()
}

impl Session {
    /// Starts the client and runs the first exchange: whatever it says on connecting (the TLS
    /// posture, the public-mode banner, a registration question...) up to its first PROMPT.
    pub async fn start(id: u64, launch: Launch, observer: Arc<dyn Observer>) -> Result<(Arc<Session>, Exchange), SessionError> {
        let endpoint = Endpoint::create().map_err(SessionError::Endpoint)?;

        let mut command = Command::new(&launch.program);
        command
            .args(&launch.args)
            .arg("--ipc")
            .arg(endpoint.argument())
            .current_dir(&launch.working_dir)
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .kill_on_drop(true);
        #[cfg(windows)]
        {
            // A console program started from a GUI would otherwise open a console window.
            const CREATE_NO_WINDOW: u32 = 0x0800_0000;
            command.creation_flags(CREATE_NO_WINDOW);
        }

        let mut child = command.spawn().map_err(|source| SessionError::Spawn {
            program: launch.program.display().to_string(),
            source,
        })?;
        let pid = child.id().ok_or_else(|| SessionError::ExitedEarly { code: None, stderr: vec![] })?;

        let stderr_tail: StderrTail = Arc::new(StdMutex::new(VecDeque::new()));
        if let Some(stderr) = child.stderr.take() {
            drain_lines(stderr, Some(stderr_tail.clone()));
        }
        if let Some(stdout) = child.stdout.take() {
            drain_lines(stdout, None); // only "Client closed" lives here; it must not block the pipe
        }

        let stream = tokio::select! {
            accepted = endpoint.accept(pid) => accepted.map_err(SessionError::Accept),
            status = child.wait() => {
                // Give the stderr reader a moment to catch the reason before reporting it.
                tokio::time::sleep(Duration::from_millis(100)).await;
                Err(SessionError::ExitedEarly {
                    code: status.ok().and_then(|s| s.code()),
                    stderr: snapshot(&stderr_tail),
                })
            }
            _ = tokio::time::sleep(ACCEPT_TIMEOUT) => Err(SessionError::AcceptTimeout(ACCEPT_TIMEOUT)),
        };
        let stream = match stream {
            Ok(stream) => stream,
            Err(e) => {
                let _ = child.kill().await;
                return Err(e);
            }
        };
        drop(endpoint); // connected; the socket file has served its purpose

        let (reader, writer) = tokio::io::split(stream);
        let (tx, rx) = mpsc::unbounded_channel();
        let (finished_tx, finished_rx) = watch::channel(None);
        let stop = Arc::new(Notify::new());

        tokio::spawn(pump(Pump {
            id,
            reader,
            tx,
            child,
            stderr_tail,
            observer,
            finished: finished_tx,
            stop: stop.clone(),
        }));

        // If this future is dropped before the first exchange completes (the user cancelled a
        // connect that was hanging), the pump must not be left reading on behalf of nobody: closing
        // the channel makes the client exit, and the pump then reaps it as usual.
        let mut abandon = StopOnDrop(Some(stop.clone()));

        let session = Arc::new(Session {
            id,
            writer: Mutex::new(Some(writer)),
            stop,
            conversation: Mutex::new(Conversation { incoming: rx, prompt: None }),
            finished: finished_rx,
        });

        let first = {
            let mut conversation = session.conversation.lock().await;
            collect(&mut conversation).await
        };
        abandon.0 = None;
        Ok((session, first))
    }

    pub fn id(&self) -> u64 {
        self.id
    }

    /// Runs one command. Refused unless the client is at a command prompt.
    pub async fn command(&self, line: &str) -> Result<Exchange, SessionError> {
        self.exchange(line, &[PromptKind::Command], "a command").await
    }

    /// Answers a password or confirm prompt. Refused unless the client is asking one.
    pub async fn respond(&self, answer: &str) -> Result<Exchange, SessionError> {
        self.exchange(answer, &[PromptKind::Password, PromptKind::Confirm], "an answer").await
    }

    async fn exchange(&self, line: &str, allowed: &[PromptKind], offered: &'static str) -> Result<Exchange, SessionError> {
        let mut conversation = self.conversation.lock().await;
        let prompt = conversation.prompt.clone().ok_or(SessionError::Ended)?;
        if !allowed.contains(&prompt.kind) {
            return Err(SessionError::WrongPrompt { expected: prompt.kind, offered });
        }

        {
            let mut writer = self.writer.lock().await;
            let writer = writer.as_mut().ok_or(SessionError::Ended)?;
            write_command(writer, line).await?;
        }
        conversation.prompt = None;
        Ok(collect(&mut conversation).await)
    }

    /// The prompt the client is currently at, if the session is alive and idle.
    pub async fn prompt(&self) -> Option<Prompt> {
        match self.conversation.try_lock() {
            Ok(conversation) => conversation.prompt.clone(),
            Err(_) => None, // an exchange is running
        }
    }

    /// Ends the session by closing the channel, which the client treats exactly like EXIT: an
    /// in-flight transfer is aborted with its state saved, so it is offered for resume at the next
    /// login. Deliberately does not wait for the conversation lock, so it also stops a transfer
    /// that is still running.
    pub async fn close(&self) -> Ending {
        if let Some(mut writer) = self.writer.lock().await.take() {
            let _ = writer.shutdown().await;
        }
        self.stop.notify_one(); // stores a permit if the reader is mid-frame, so it is never lost
        self.wait_finished().await
    }

    pub async fn wait_finished(&self) -> Ending {
        let mut finished = self.finished.clone();
        loop {
            if let Some(ending) = finished.borrow().clone() {
                return ending;
            }
            if finished.changed().await.is_err() {
                return finished.borrow().clone().unwrap_or(Ending { exit_code: None, stderr: vec![], last_error: None });
            }
        }
    }
}

async fn collect(conversation: &mut Conversation) -> Exchange {
    let mut frames = Vec::new();
    loop {
        match conversation.incoming.recv().await {
            Some(Incoming::Frame(frame)) => frames.push(frame),
            Some(Incoming::Prompt(prompt)) => {
                conversation.prompt = Some(prompt.clone());
                return Exchange { frames, prompt: Some(prompt) };
            }
            None => {
                conversation.prompt = None;
                return Exchange { frames, prompt: None };
            }
        }
    }
}

/// Everything the reading side of a session owns: the channel's read half, the client process, and
/// where to report frames and the ending.
struct Pump {
    id: u64,
    reader: tokio::io::ReadHalf<IpcStream>,
    tx: mpsc::UnboundedSender<Incoming>,
    child: Child,
    stderr_tail: StderrTail,
    observer: Arc<dyn Observer>,
    finished: watch::Sender<Option<Ending>>,
    stop: Arc<Notify>,
}

/// Reads frames until the channel closes, then reaps the client and reports how it ended.
async fn pump(p: Pump) {
    let Pump { id, mut reader, tx, mut child, stderr_tail, observer, finished, stop } = p;
    let mut last_error = None;
    loop {
        let next = tokio::select! {
            frame = read_frame(&mut reader) => frame,
            _ = stop.notified() => break,
        };
        match next {
            Ok(value) => match parse(value) {
                Some(Incoming::Frame(frame)) => {
                    if let Frame::Result { ok: false, message, .. } = &frame {
                        last_error = Some(message.clone());
                    }
                    observer.frame(id, &frame);
                    let _ = tx.send(Incoming::Frame(frame));
                }
                Some(prompt) => {
                    let _ = tx.send(prompt);
                }
                None => {}
            },
            Err(_) => break,
        }
    }
    drop(reader); // with the writer already gone, this closes the channel on every platform
    drop(tx); // any exchange waiting now completes with prompt = None

    let exit_code = match tokio::time::timeout(EXIT_GRACE, child.wait()).await {
        Ok(Ok(status)) => status.code(),
        _ => {
            let _ = child.kill().await;
            child.wait().await.ok().and_then(|s| s.code())
        }
    };
    tokio::time::sleep(Duration::from_millis(50)).await; // let the stderr reader drain
    let ending = Ending { exit_code, stderr: snapshot(&stderr_tail), last_error };
    observer.ended(id, &ending);
    let _ = finished.send(Some(ending));
}
