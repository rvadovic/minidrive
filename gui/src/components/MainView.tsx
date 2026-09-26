import { useState } from "react";
import { useSession } from "../session";
import { useFeedback } from "./Feedback";
import { FilesView } from "./FilesView";
import { SyncView } from "./SyncView";
import { StorageView } from "./StorageView";
import { VaultView } from "./VaultView";
import { ActivityView } from "./ActivityView";
import { baseName } from "../lib/paths";
import { formatBytes, percent } from "../lib/format";

type Tab = "files" | "sync" | "storage" | "vault" | "activity";

const TABS: { id: Tab; label: string }[] = [
  { id: "files", label: "Files" },
  { id: "sync", label: "Sync" },
  { id: "storage", label: "Storage" },
  { id: "vault", label: "Vault" },
  { id: "activity", label: "Activity" },
];

function PostureBadge() {
  const { posture } = useSession();
  if (!posture.encrypted) {
    return (
      <span className="badge badge-bad" title="Rung 0: your password and files cross the network in plain text." data-testid="posture">
        Not encrypted
      </span>
    );
  }
  const title = `${posture.protocol ?? "TLS"} · ${posture.cipher ?? "unknown cipher"} · key exchange ${posture.group ?? "unknown"}`;
  return (
    <span className={`badge ${posture.postQuantum ? "badge-ok" : "badge-neutral"}`} title={title} data-testid="posture">
      {posture.protocol?.replace("v", " ") ?? "TLS"} · {posture.postQuantum ? "post-quantum" : "classical key exchange"}
    </span>
  );
}

function VaultBadge({ onClick }: { onClick: () => void }) {
  const { vault, profile } = useSession();
  if (!profile?.username || !vault) return null;
  if (vault.enabled && vault.unlocked) return <button className="badge badge-ok as-button" onClick={onClick}>End-to-end encrypted</button>;
  if (vault.enabled) return <button className="badge badge-warn as-button" onClick={onClick}>Vault locked</button>;
  return <button className="badge badge-neutral as-button" onClick={onClick}>Vault off</button>;
}

function TransferBar() {
  const { busy, transfer, batch, disconnect } = useSession();
  const feedback = useFeedback();
  if (busy === 0) return null;

  const active = transfer && transfer.chunks_done < transfer.chunks_total ? transfer : null;
  const name = active ? baseName(active.direction === "upload" ? active.remote_path || active.path : active.local_path || active.path) : "";

  const stop = async () => {
    const yes = await feedback.confirm({
      title: "Stop and disconnect?",
      body: "MiniDrive stops by ending this session. A transfer that is interrupted this way is saved, and you are offered to resume it the next time you sign in.",
      action: "Stop",
      danger: true,
    });
    if (yes) await disconnect();
  };

  return (
    <footer className="transfer-bar" data-testid="transfer-bar">
      <div className="spinner" aria-hidden />
      <div className="transfer-text">
        {batch && <span className="batch">{batch.label}: item {batch.index} of {batch.total} · </span>}
        {active ? (
          <>
            {active.direction === "upload" ? "Uploading" : "Downloading"} <strong>{name}</strong>{" "}
            <span className="hint">{formatBytes(active.bytes_total)}</span>
          </>
        ) : (
          "Working…"
        )}
      </div>
      {active && (
        <div className="progress" aria-label="Transfer progress">
          <div className="progress-fill" style={{ width: `${percent(active.chunks_done, active.chunks_total)}%` }} />
          <span>{percent(active.chunks_done, active.chunks_total)}%</span>
        </div>
      )}
      <button className="btn btn-small" onClick={stop}>Stop</button>
    </footer>
  );
}

export function MainView() {
  const { profile, disconnect } = useSession();
  const [tab, setTab] = useState<Tab>("files");
  const account = profile ? `${profile.username || "public"}@${profile.host}:${profile.port}` : "";

  return (
    <div className="main-view">
      <header className="top-bar">
        <div className="brand">
          <img src="/icon.png" alt="" width={24} height={24} />
          <span>MiniDrive</span>
        </div>
        <div className="account" data-testid="account">{account}</div>
        <PostureBadge />
        <VaultBadge onClick={() => setTab("vault")} />
        <span className="spacer" />
        <button className="btn btn-small" onClick={() => disconnect()} data-testid="disconnect">Disconnect</button>
      </header>
      <div className="body">
        <nav className="side-nav">
          {TABS.map((t) => (
            <button key={t.id} className={tab === t.id ? "active" : ""} onClick={() => setTab(t.id)} data-testid={`tab-${t.id}`}>
              {t.label}
            </button>
          ))}
        </nav>
        <section className="content">
          {/* Files stays mounted so its tree and position survive a visit to another tab. */}
          <div className={tab === "files" ? "tab-pane" : "tab-pane hidden"}><FilesView /></div>
          {tab === "sync" && <SyncView />}
          {tab === "storage" && <StorageView />}
          {tab === "vault" && <VaultView />}
          {tab === "activity" && <ActivityView />}
        </section>
      </div>
      <TransferBar />
    </div>
  );
}
