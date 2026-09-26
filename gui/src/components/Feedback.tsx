import { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState, type ReactNode } from "react";

// Two kinds of feedback the GUI raises on its own behalf - as opposed to questions the *client*
// asks, which the session routes through QuestionDialog:
//   - toasts, for outcomes ("Uploaded 3 files", "Permission denied");
//   - confirmations, for decisions the GUI needs before acting ("Replace the existing file?").

interface Toast {
  id: number;
  kind: "ok" | "error" | "info";
  text: string;
}

interface ConfirmRequest {
  title: string;
  body: ReactNode;
  action: string;
  danger?: boolean;
  resolve: (yes: boolean) => void;
}

interface FeedbackApi {
  ok(text: string): void;
  error(text: string): void;
  info(text: string): void;
  confirm(request: Omit<ConfirmRequest, "resolve">): Promise<boolean>;
}

const Ctx = createContext<FeedbackApi | null>(null);

export function useFeedback(): FeedbackApi {
  const value = useContext(Ctx);
  if (!value) throw new Error("useFeedback outside FeedbackProvider");
  return value;
}

export function FeedbackProvider({ children }: { children: ReactNode }) {
  const [toasts, setToasts] = useState<Toast[]>([]);
  const [confirming, setConfirming] = useState<ConfirmRequest | null>(null);
  const nextId = useRef(0);

  const push = useCallback((kind: Toast["kind"], text: string) => {
    const id = ++nextId.current;
    setToasts((prev) => [...prev.slice(-4), { id, kind, text }]);
    setTimeout(() => setToasts((prev) => prev.filter((t) => t.id !== id)), kind === "error" ? 8000 : 4000);
  }, []);

  // Stable for the life of the app: views put this in their effect dependencies, and a new object
  // per render (every toast re-renders this provider) would re-run those effects each time.
  const api = useMemo<FeedbackApi>(
    () => ({
      ok: (text) => push("ok", text),
      error: (text) => push("error", text),
      info: (text) => push("info", text),
      confirm: (request) => new Promise<boolean>((resolve) => setConfirming({ ...request, resolve })),
    }),
    [push],
  );

  const answer = (yes: boolean) => {
    confirming?.resolve(yes);
    setConfirming(null);
  };

  return (
    <Ctx.Provider value={api}>
      {children}
      <div className="toasts" role="status" aria-live="polite">
        {toasts.map((t) => (
          <div key={t.id} className={`toast toast-${t.kind}`} onClick={() => setToasts((p) => p.filter((x) => x.id !== t.id))}>
            {t.text}
          </div>
        ))}
      </div>
      {confirming && (
        <Modal title={confirming.title} onCancel={() => answer(false)}>
          <div className="modal-body">{confirming.body}</div>
          <div className="modal-actions">
            <button className="btn" onClick={() => answer(false)}>Cancel</button>
            <button className={`btn ${confirming.danger ? "btn-danger" : "btn-primary"}`} onClick={() => answer(true)} autoFocus>
              {confirming.action}
            </button>
          </div>
        </Modal>
      )}
    </Ctx.Provider>
  );
}

export function Modal({ title, children, onCancel }: { title: string; children: ReactNode; onCancel?: () => void }) {
  useEffect(() => {
    if (!onCancel) return;
    const onKey = (e: KeyboardEvent) => e.key === "Escape" && onCancel();
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [onCancel]);

  return (
    <div className="modal-backdrop">
      <div className="modal" role="dialog" aria-modal="true" aria-label={title}>
        <h2 className="modal-title">{title}</h2>
        {children}
      </div>
    </div>
  );
}
