import { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState, type ReactNode } from "react";
import { api, onEnded, onFrame } from "./api";
import { parsePosture, PLAINTEXT, type Posture } from "./lib/posture";
import type {
  BatchItem,
  BatchSummary,
  Ending,
  Exchange,
  Frame,
  Operation,
  Profile,
  Prompt,
  TransferProgress,
  VaultStatus,
} from "./types";

// The conversation, from the frontend's side.
//
// Every operation goes through one queue, so they reach the client one at a time. An operation is
// finished only when the client is back at a *command* prompt: if it stops at a question
// ("Register?", "Resume this upload?", "Move your data to archive?") or asks for the password,
// the question is put in front of the user and their answer sent, until it is.

export interface OpResult {
  ok: boolean;
  code: number;
  message: string;
  frames: Frame[];
  events: (name: string) => Record<string, unknown>[];
}

export interface Question {
  kind: "password" | "confirm";
  text: string;
  /** The last error the client reported before asking again (wrong password, lockout). */
  error?: string;
  resolve: (answer: string | null) => void;
}

export interface LogEntry {
  id: number;
  time: Date;
  kind: "ok" | "error" | "info";
  text: string;
}

export class SessionEnded extends Error {}

// The server asks its questions with a non-200 code (401 "please provide a password", 401 "Do you
// want to register? (Y/n)"), and a RESULT frame carries only the code - so a question looks like a
// failure. These are recognised by shape and treated as information; a real refusal ("Invalid
// password. Try again.") still reads as an error.
export function isQuestion(message: string): boolean {
  return /\((y|Y)\/(n|N)\)\s*$/.test(message) || /please provide/i.test(message);
}

// Read-only operations the GUI issues on its own - every folder visit is a LIST. Their output is
// the CLI's text rendering of what the view already draws, so only their errors are logged.
const QUIET_OPS = new Set<Operation["op"]>(["list", "tiers", "devices", "vault_status"]);

type Phase = "disconnected" | "connecting" | "ready";

interface SessionApi {
  phase: Phase;
  profile: Profile | null;
  posture: Posture;
  vault: VaultStatus | null;
  busy: number;
  transfer: TransferProgress | null;
  batch: BatchItem | null;
  lastSummary: BatchSummary | null;
  log: LogEntry[];
  question: Question | null;
  endedReason: { title: string; detail: string } | null;
  connect(profile: Profile): Promise<void>;
  /** Stops a connect in progress: before the client is up, or while it is signing in / resuming. */
  cancelConnect(): Promise<void>;
  disconnect(): Promise<void>;
  run(op: Operation): Promise<OpResult>;
  refreshVault(): Promise<VaultStatus | null>;
  clearEndedReason(): void;
}

const Ctx = createContext<SessionApi | null>(null);

export function useSession(): SessionApi {
  const value = useContext(Ctx);
  if (!value) throw new Error("useSession outside SessionProvider");
  return value;
}

function summarize(exchanges: Exchange[]): OpResult {
  const frames = exchanges.flatMap((e) => e.frames);
  const results = frames.filter((f): f is Extract<Frame, { type: "result" }> => f.type === "result");
  const last = results[results.length - 1];
  return {
    ok: last ? last.ok : true,
    code: last ? last.code : 200,
    message: last ? last.message : "",
    frames,
    events: (name) =>
      frames.flatMap((f) => (f.type === "event" && f.name === name ? [f.data] : [])),
  };
}

function lastError(exchange: Exchange): string | undefined {
  for (let i = exchange.frames.length - 1; i >= 0; i--) {
    const f = exchange.frames[i];
    if (f.type === "result" && !f.ok && !isQuestion(f.message)) return f.message;
  }
  return undefined;
}

function describeEnding(ending: Ending): string {
  const lines = [...ending.stderr];
  if (ending.last_error && !lines.some((l) => l.includes(ending.last_error!))) lines.push(ending.last_error);
  return lines.length ? lines.join("\n") : "The connection to the server was closed.";
}

export function SessionProvider({ children }: { children: ReactNode }) {
  const [phase, setPhase] = useState<Phase>("disconnected");
  const [profile, setProfile] = useState<Profile | null>(null);
  const [posture, setPosture] = useState<Posture>(PLAINTEXT);
  const [vault, setVault] = useState<VaultStatus | null>(null);
  const [busy, setBusy] = useState(0);
  const [transfer, setTransfer] = useState<TransferProgress | null>(null);
  const [batch, setBatch] = useState<BatchItem | null>(null);
  const [lastSummary, setLastSummary] = useState<BatchSummary | null>(null);
  const [log, setLog] = useState<LogEntry[]>([]);
  const [question, setQuestion] = useState<Question | null>(null);
  const [endedReason, setEndedReason] = useState<{ title: string; detail: string } | null>(null);

  const sessionId = useRef<number | null>(null);
  const closing = useRef(false);
  const chain = useRef<Promise<unknown>>(Promise.resolve());
  const logId = useRef(0);
  const quiet = useRef(false); // the operation in flight is one of QUIET_OPS

  const append = useCallback((kind: LogEntry["kind"], text: string) => {
    setLog((prev) => [...prev.slice(-499), { id: ++logId.current, time: new Date(), kind, text }]);
  }, []);

  // Live frames: the activity log and the progress bars move while an operation is still running.
  useEffect(() => {
    const unlisten = onFrame((_session, frame) => {
      if (quiet.current && (frame.type === "info" || (frame.type === "result" && frame.ok))) return;
      if (frame.type === "result") {
        if (frame.ok || isQuestion(frame.message)) append(frame.ok ? "ok" : "info", frame.message);
        else append("error", `${frame.message} (${frame.code})`);
      } else if (frame.type === "info") {
        append("info", frame.text);
      } else if (frame.name === "transfer_progress") {
        setTransfer(frame.data as unknown as TransferProgress);
      } else if (frame.name === "batch_item") {
        setBatch(frame.data as unknown as BatchItem);
      } else if (frame.name === "batch_summary") {
        setBatch(null);
        setLastSummary(frame.data as unknown as BatchSummary);
      } else if (frame.name === "vault_status") {
        setVault(frame.data as unknown as VaultStatus);
      }
    });
    return () => void unlisten.then((f) => f());
  }, [append]);

  const reset = useCallback(() => {
    sessionId.current = null;
    setPhase("disconnected");
    setVault(null);
    setTransfer(null);
    setBatch(null);
    setQuestion((q) => {
      q?.resolve(null);
      return null;
    });
    chain.current = Promise.resolve();
  }, []);

  useEffect(() => {
    const unlisten = onEnded((session, ending) => {
      if (session !== sessionId.current) return;
      if (!closing.current) {
        const reason = describeEnding(ending);
        append("error", `Disconnected: ${reason}`);
        setEndedReason({ title: "Disconnected from the server.", detail: reason });
      }
      reset();
    });
    return () => void unlisten.then((f) => f());
  }, [append, reset]);

  const ask = useCallback((prompt: Prompt, error?: string) => {
    return new Promise<string | null>((resolve) => {
      setQuestion({
        kind: prompt.kind === "password" ? "password" : "confirm",
        text: prompt.text,
        error,
        resolve: (answer) => {
          setQuestion(null);
          resolve(answer);
        },
      });
    });
  }, []);

  const disconnect = useCallback(async () => {
    closing.current = true;
    try {
      await api.disconnect();
    } finally {
      closing.current = false;
      reset();
    }
  }, [reset]);

  // Answers questions until the client is back at a command prompt.
  const drive = useCallback(
    async (first: Exchange): Promise<Exchange[]> => {
      const all = [first];
      let current = first;
      while (current.prompt && current.prompt.kind !== "command") {
        let answer = await ask(current.prompt, lastError(current));
        if (answer === null) {
          if (current.prompt.kind === "password") {
            // A password prompt cannot be declined; leaving is the only way out of it.
            await disconnect();
            throw new SessionEnded("Login cancelled.");
          }
          answer = "n";
        }
        current = await api.respond(answer);
        all.push(current);
      }
      if (!current.prompt) throw new SessionEnded("The session ended.");
      return all;
    },
    [ask, disconnect],
  );

  const run = useCallback(
    (op: Operation): Promise<OpResult> => {
      const task = chain.current.then(async () => {
        if (sessionId.current === null) throw new SessionEnded("Not connected.");
        setBusy((b) => b + 1);
        quiet.current = QUIET_OPS.has(op.op); // safe: operations never overlap
        try {
          return summarize(await drive(await api.execute(op)));
        } finally {
          quiet.current = false;
          setBusy((b) => b - 1);
        }
      });
      chain.current = task.catch(() => undefined);
      return task;
    },
    [drive],
  );

  const refreshVault = useCallback(async () => {
    const result = await run({ op: "vault_status" });
    const status = result.events("vault_status")[0] as unknown as VaultStatus | undefined;
    if (status) setVault(status);
    return status ?? null;
  }, [run]);

  const cancelled = useRef(false);

  const cancelConnect = useCallback(async () => {
    cancelled.current = true;
    if (sessionId.current === null) await api.cancelConnect();
    else await disconnect(); // already up: signing in, or resuming a transfer
  }, [disconnect]);

  const connect = useCallback(
    async (target: Profile) => {
      cancelled.current = false;
      setEndedReason(null);
      setPhase("connecting");
      setProfile(target);
      setPosture(PLAINTEXT);
      setLastSummary(null);
      try {
        append("info", `Connecting to ${target.username || "public"}@${target.host}:${target.port}...`);
        const { session, exchange } = await api.connect(target);
        sessionId.current = session;
        const exchanges = await drive(exchange);
        for (const frame of exchanges.flatMap((e) => e.frames)) {
          if (frame.type === "result") {
            const p = parsePosture(frame.message);
            if (p) setPosture(p);
          }
        }
        setPhase("ready");
        await refreshVault().catch(() => null);
      } catch (e) {
        if (cancelled.current) {
          append("info", "Connection cancelled.");
        } else if (!(e instanceof SessionEnded)) {
          const reason = String(e);
          append("error", reason);
          setEndedReason({ title: "Could not connect.", detail: reason });
        }
        await api.disconnect().catch(() => null);
        reset();
      }
    },
    [append, drive, refreshVault, reset],
  );

  const value = useMemo<SessionApi>(
    () => ({
      phase,
      profile,
      posture,
      vault,
      busy,
      transfer,
      batch,
      lastSummary,
      log,
      question,
      endedReason,
      connect,
      cancelConnect,
      disconnect,
      run,
      refreshVault,
      clearEndedReason: () => setEndedReason(null),
    }),
    [phase, profile, posture, vault, busy, transfer, batch, lastSummary, log, question, endedReason, connect, cancelConnect, disconnect, run, refreshVault],
  );

  return <Ctx.Provider value={value}>{children}</Ctx.Provider>;
}
