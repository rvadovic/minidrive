import { useState, type FormEvent } from "react";
import { useSession } from "../session";
import { Modal } from "./Feedback";

// Questions the *client* is asking - the password, or a y/n the server put to it (register this
// account? resume this transfer? migrate to another tier? revoke this device?). The text is the
// server's own; the GUI adds buttons, not meaning.

function stripYesNo(text: string): string {
  return text.replace(/\s*\((y|Y)\/(n|N)\)\s*$/, "").trim();
}

function confirmTitle(text: string): string {
  if (/register/i.test(text)) return "Create account?";
  if (/resume/i.test(text)) return "Resume interrupted transfer?";
  if (/tier/i.test(text)) return "Move your data?";
  if (/revoke/i.test(text)) return "Revoke device?";
  return "Please confirm";
}

export function QuestionDialog() {
  const { question, profile } = useSession();
  const [password, setPassword] = useState("");

  if (!question) return null;

  if (question.kind === "password") {
    const submit = (e: FormEvent) => {
      e.preventDefault();
      const value = password;
      setPassword(""); // the field never outlives the submission
      question.resolve(value);
    };
    const label = question.text.replace(/:\s*$/, "") || `Password for ${profile?.username ?? "this account"}`;
    return (
      <Modal title="Sign in" onCancel={() => question.resolve(null)}>
        <form onSubmit={submit}>
          <label className="field">
            <span>{label}</span>
            <input
              type="password"
              value={password}
              autoFocus
              autoComplete="current-password"
              onChange={(e) => setPassword(e.target.value)}
              data-testid="password-input"
            />
          </label>
          {question.error && <p className="callout callout-error">{question.error}</p>}
          <p className="hint">
            Your password is sent only to the MiniDrive client running on this computer
            {profile?.security.tls ? ", which sends it to the server over TLS." : ". This connection is not encrypted."}
          </p>
          <div className="modal-actions">
            <button type="button" className="btn" onClick={() => question.resolve(null)}>Cancel</button>
            <button type="submit" className="btn btn-primary" disabled={password.length === 0} data-testid="password-submit">
              Sign in
            </button>
          </div>
        </form>
      </Modal>
    );
  }

  return (
    <Modal title={confirmTitle(question.text)} onCancel={() => question.resolve("n")}>
      <p className="modal-body" data-testid="question-text">{stripYesNo(question.text) || "The server is asking for confirmation."}</p>
      {question.error && <p className="callout callout-error">{question.error}</p>}
      <div className="modal-actions">
        <button className="btn" onClick={() => question.resolve("n")} data-testid="question-no">No</button>
        <button className="btn btn-primary" onClick={() => question.resolve("y")} autoFocus data-testid="question-yes">
          Yes
        </button>
      </div>
    </Modal>
  );
}
