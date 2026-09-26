import { useState } from "react";
import { open } from "@tauri-apps/plugin-dialog";
import { useSession, SessionEnded } from "../session";
import { useProfiles } from "../profiles";
import { useFeedback } from "./Feedback";
import { baseName, normalizeRemote } from "../lib/paths";
import type { BatchSummary, SyncPair } from "../types";

// SYNC is the client's three-way merge against a baseline stored inside the local folder
// (.minidrive-sync/). The GUI only remembers which folder pairs to sync; every decision - upload,
// download, move, conflict copy - is the client's.

interface PairOutcome {
  summary?: BatchSummary;
  error?: string;
  at: Date;
}

function key(pair: SyncPair) {
  return `${pair.local}\u0000${pair.remote}`;
}

export function SyncView() {
  const { run, profile } = useSession();
  const { profiles, save } = useProfiles();
  const feedback = useFeedback();
  const [outcomes, setOutcomes] = useState<Record<string, PairOutcome>>({});
  const [remoteDraft, setRemoteDraft] = useState("");
  const [localDraft, setLocalDraft] = useState("");

  // The profile in the store is the one to edit; the session holds the copy it connected with.
  const stored = profiles.find((p) => p.id === profile?.id) ?? profile;
  const pairs = stored?.syncPairs ?? [];

  const persistPairs = async (next: SyncPair[]) => {
    if (!stored) return;
    try {
      await save({ ...stored, syncPairs: next });
    } catch (e) {
      feedback.error(String(e));
    }
  };

  const syncPair = async (pair: SyncPair) => {
    try {
      // SYNC never creates its own remote root; creating it first makes the first sync of a new
      // pair just work. "Already exists" is the normal case and is not an error here.
      await run({ op: "mkdir", path: pair.remote });
      const result = await run({ op: "sync", local: pair.local, remote: pair.remote });
      const summary = result.events("batch_summary")[0] as unknown as BatchSummary | undefined;
      setOutcomes((o) => ({ ...o, [key(pair)]: { summary, error: result.ok ? undefined : result.message, at: new Date() } }));
      if (!result.ok) feedback.error(result.message);
      else if (summary && summary.conflicts.length) feedback.info(`${summary.conflicts.length} conflict(s): both copies were kept.`);
    } catch (e) {
      if (!(e instanceof SessionEnded)) feedback.error(String(e));
    }
  };

  const chooseLocal = async () => {
    const chosen = await open({ directory: true, multiple: false, title: "Folder on this computer to keep in sync" });
    if (typeof chosen === "string") {
      setLocalDraft(chosen);
      if (!remoteDraft) setRemoteDraft(`/${baseName(chosen)}`);
    }
  };

  const addPair = async () => {
    const pair = { local: localDraft, remote: normalizeRemote(remoteDraft || `/${baseName(localDraft)}`) };
    if (!pair.local) return;
    if (pairs.some((p) => key(p) === key(pair))) {
      feedback.error("That pair is already in the list.");
      return;
    }
    await persistPairs([...pairs, pair]);
    setLocalDraft("");
    setRemoteDraft("");
  };

  return (
    <div className="panel-view">
      <header className="view-header">
        <h2>Sync</h2>
        <p className="hint">
          Keeps a folder on this computer and a folder on the server identical. Changes on either side are carried to the
          other; when both sides changed the same file, the server's version is saved next to yours as a conflict copy, so
          nothing is lost.
        </p>
      </header>

      <div className="card">
        <h3>Add a folder</h3>
        <div className="form-grid">
          <label className="field span-2">
            <span>On this computer</span>
            <div className="with-button">
              <input value={localDraft} readOnly placeholder="Choose a folder…" />
              <button className="btn" onClick={chooseLocal}>Browse…</button>
            </div>
          </label>
          <label className="field span-2">
            <span>On the server</span>
            <input value={remoteDraft} placeholder="/Documents" onChange={(e) => setRemoteDraft(e.target.value)} />
          </label>
        </div>
        <div className="card-actions">
          <button className="btn btn-primary" disabled={!localDraft} onClick={addPair}>Add</button>
        </div>
      </div>

      {pairs.length === 0 ? (
        <p className="empty">No folders are set up to sync yet.</p>
      ) : (
        <>
          <div className="row-actions">
            <button className="btn btn-primary" onClick={async () => { for (const p of pairs) await syncPair(p); }}>
              Sync all now
            </button>
          </div>
          <ul className="pair-list">
            {pairs.map((pair) => {
              const outcome = outcomes[key(pair)];
              const s = outcome?.summary;
              return (
                <li key={key(pair)} className="card pair">
                  <div className="pair-paths">
                    <code title={pair.local}>{pair.local}</code>
                    <span aria-hidden>⇄</span>
                    <code>{pair.remote}</code>
                  </div>
                  {outcome && (
                    <div className={`pair-outcome ${outcome.error ? "bad" : ""}`}>
                      {outcome.error
                        ? outcome.error
                        : s
                          ? `Synced at ${outcome.at.toLocaleTimeString()}: ${s.uploaded} up, ${s.downloaded} down, ${s.deleted} deleted, ${s.moved} moved, ${s.skipped} unchanged${s.failed ? `, ${s.failed} failed` : ""}.`
                          : `Synced at ${outcome.at.toLocaleTimeString()}.`}
                      {s && s.conflicts.length > 0 && (
                        <ul className="conflicts">
                          {s.conflicts.map((c) => <li key={c}>{c}</li>)}
                        </ul>
                      )}
                    </div>
                  )}
                  <div className="card-actions">
                    <button className="btn btn-danger-quiet" onClick={() => persistPairs(pairs.filter((p) => key(p) !== key(pair)))}>
                      Remove
                    </button>
                    <span className="spacer" />
                    <button className="btn btn-primary" onClick={() => syncPair(pair)}>Sync now</button>
                  </div>
                </li>
              );
            })}
          </ul>
        </>
      )}
    </div>
  );
}
