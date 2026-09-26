import { useEffect, useState } from "react";
import { open } from "@tauri-apps/plugin-dialog";
import { newProfile, useProfiles } from "../profiles";
import { useSession } from "../session";
import { baseName } from "../lib/paths";
import { percent } from "../lib/format";
import type { Profile } from "../types";

function label(p: Profile): string {
  return p.name || `${p.username || "public"}@${p.host}:${p.port}`;
}

export function ConnectScreen() {
  const { profiles, loaded, error: loadError, save, remove } = useProfiles();
  const { connect, cancelConnect, phase, endedReason, clearEndedReason, transfer } = useSession();
  const [draft, setDraft] = useState<Profile | null>(null);
  const [formError, setFormError] = useState<string | null>(null);

  useEffect(() => {
    if (loaded && draft === null) setDraft(profiles[0] ? structuredClone(profiles[0]) : newProfile());
  }, [loaded, profiles, draft]);

  if (!draft) return <div className="connect-screen" />;

  const connecting = phase === "connecting";
  const update = (patch: Partial<Profile>) => setDraft({ ...draft, ...patch });
  const security = (patch: Partial<Profile["security"]>) => setDraft({ ...draft, security: { ...draft.security, ...patch } });

  const persist = async (): Promise<Profile | null> => {
    const candidate = { ...draft, name: draft.name.trim() || `${draft.host}:${draft.port}`, host: draft.host.trim() };
    try {
      await save(candidate);
      setDraft(candidate);
      setFormError(null);
      return candidate;
    } catch (e) {
      setFormError(String(e));
      return null;
    }
  };

  const onConnect = async () => {
    clearEndedReason();
    const saved = await persist();
    if (saved) await connect(saved);
  };

  const pickFile = async (setter: (path: string) => void, directory = false) => {
    const chosen = await open({ multiple: false, directory });
    if (typeof chosen === "string") setter(chosen);
  };

  const noTrustAnchor = draft.security.tls && !draft.security.caFile && !draft.security.pin;

  return (
    <div className="connect-screen">
      <aside className="server-list">
        <div className="brand">
          <img src="/icon.png" alt="" width={28} height={28} />
          <span>MiniDrive</span>
        </div>
        <h3>Servers</h3>
        <ul>
          {profiles.map((p) => (
            <li key={p.id}>
              <button className={`server-item ${p.id === draft.id ? "active" : ""}`} onClick={() => setDraft(structuredClone(p))}>
                <span className="server-name">{label(p)}</span>
                <span className="server-sub">
                  {p.username || "public area"} · {p.security.tls ? "TLS" : "plaintext"}
                </span>
              </button>
            </li>
          ))}
        </ul>
        <button className="btn btn-block" onClick={() => setDraft(newProfile())} data-testid="new-server">
          + New server
        </button>
        {loadError && <p className="callout callout-error">{loadError}</p>}
      </aside>

      <main className="server-editor">
        {endedReason && (
          <div className="callout callout-error" data-testid="connect-error">
            <strong>{endedReason.title}</strong>
            <pre>{endedReason.detail}</pre>
          </div>
        )}

        <h2>{profiles.some((p) => p.id === draft.id) ? label(draft) : "New server"}</h2>

        <div className="form-grid">
          <label className="field span-2">
            <span>Name</span>
            <input value={draft.name} placeholder="Home NAS" onChange={(e) => update({ name: e.target.value })} data-testid="profile-name" />
          </label>
          <label className="field">
            <span>Server address</span>
            <input value={draft.host} placeholder="nas.local or 192.168.1.20" onChange={(e) => update({ host: e.target.value })} data-testid="profile-host" />
          </label>
          <label className="field field-narrow">
            <span>Port</span>
            <input
              type="number"
              min={1}
              max={65535}
              value={draft.port}
              onChange={(e) => update({ port: Number(e.target.value) })}
              data-testid="profile-port"
            />
          </label>
          <label className="field span-2">
            <span>Username</span>
            <input
              value={draft.username}
              placeholder="Leave empty for the shared public area"
              autoCapitalize="off"
              spellCheck={false}
              onChange={(e) => update({ username: e.target.value })}
              data-testid="profile-username"
            />
          </label>
        </div>

        <fieldset className="security">
          <legend>Security</legend>
          <label className="check">
            <input type="checkbox" checked={draft.security.tls} onChange={(e) => security({ tls: e.target.checked })} data-testid="profile-tls" />
            <span>Encrypt the connection (TLS 1.3, with post-quantum key exchange where the server supports it)</span>
          </label>

          {!draft.security.tls && (
            <p className="callout callout-warn">
              Without TLS your password and every file cross the network in plain text. Only use this on a network you fully
              control, or for testing.
            </p>
          )}

          {draft.security.tls && (
            <div className="form-grid">
              <label className="field span-2">
                <span>CA certificate (the <code>ca.crt</code> your server was set up with)</span>
                <div className="with-button">
                  <input value={draft.security.caFile ?? ""} placeholder="Use the system's trusted CAs" onChange={(e) => security({ caFile: e.target.value || null })} />
                  <button className="btn" onClick={() => pickFile((p) => security({ caFile: p }))}>Browse…</button>
                </div>
              </label>
              <label className="field span-2">
                <span>Public-key pin (optional)</span>
                <input
                  value={draft.security.pin ?? ""}
                  placeholder="sha256:…  printed by the server at startup"
                  spellCheck={false}
                  onChange={(e) => security({ pin: e.target.value || null })}
                  data-testid="profile-pin"
                />
              </label>
              <label className="field span-2">
                <span>Certificate name (optional)</span>
                <input
                  value={draft.security.serverName ?? ""}
                  placeholder="Only if you connect by an address the certificate does not list"
                  onChange={(e) => security({ serverName: e.target.value || null })}
                />
              </label>
              <label className="check span-2">
                <input type="checkbox" checked={draft.security.requirePq} onChange={(e) => security({ requirePq: e.target.checked })} />
                <span>Refuse to connect without post-quantum key exchange (X25519MLKEM768)</span>
              </label>
              {noTrustAnchor && (
                <p className="hint span-2">
                  With no CA file or pin, the server's certificate must be issued by a CA this computer already trusts. A
                  self-hosted MiniDrive usually needs its <code>ca.crt</code> or pin here.
                </p>
              )}
            </div>
          )}
        </fieldset>

        <label className="field">
          <span>Default download folder</span>
          <div className="with-button">
            <input value={draft.downloadDir ?? ""} placeholder="Your Downloads folder" onChange={(e) => update({ downloadDir: e.target.value || null })} />
            <button className="btn" onClick={() => pickFile((p) => update({ downloadDir: p }), true)}>Browse…</button>
          </div>
        </label>

        {formError && <p className="callout callout-error">{formError}</p>}

        <div className="editor-actions">
          {profiles.some((p) => p.id === draft.id) && (
            <button
              className="btn btn-danger-quiet"
              onClick={async () => {
                await remove(draft.id);
                setDraft(newProfile());
              }}
            >
              Remove
            </button>
          )}
          <span className="spacer" />
          {connecting && transfer && transfer.chunks_done < transfer.chunks_total && (
            // A transfer resumed at sign-in runs before the main window appears.
            <span className="hint">
              Resuming {baseName(transfer.remote_path || transfer.path)}: {percent(transfer.chunks_done, transfer.chunks_total)}%
            </span>
          )}
          <button className="btn" onClick={persist} disabled={connecting}>Save</button>
          {connecting && (
            <button className="btn" onClick={() => void cancelConnect()} data-testid="cancel-connect">
              Cancel
            </button>
          )}
          <button className="btn btn-primary" onClick={onConnect} disabled={connecting || !draft.host.trim()} data-testid="connect">
            {connecting ? "Connecting…" : "Connect"}
          </button>
        </div>
      </main>
    </div>
  );
}
