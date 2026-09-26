import { useState } from "react";
import { useSession } from "../session";
import { timeOfDay } from "../lib/format";

// Everything the client reported this session, in order - the same lines the command-line client
// prints, which makes this the first place to look when something did not do what was expected.

export function ActivityView() {
  const { log } = useSession();
  const [errorsOnly, setErrorsOnly] = useState(false);
  const shown = errorsOnly ? log.filter((l) => l.kind === "error") : log;

  return (
    <div className="panel-view">
      <header className="view-header row">
        <h2>Activity</h2>
        <label className="check">
          <input type="checkbox" checked={errorsOnly} onChange={(e) => setErrorsOnly(e.target.checked)} />
          <span>Errors only</span>
        </label>
      </header>
      {shown.length === 0 ? (
        <p className="empty">Nothing yet.</p>
      ) : (
        <ol className="log" data-testid="activity-log">
          {[...shown].reverse().map((entry) => (
            <li key={entry.id} className={`log-${entry.kind}`}>
              <time>{timeOfDay(entry.time)}</time>
              <span>{entry.text}</span>
            </li>
          ))}
        </ol>
      )}
    </div>
  );
}
