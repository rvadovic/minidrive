import { useCallback, useEffect, useState } from "react";
import { useSession, SessionEnded } from "../session";
import { useFeedback } from "./Feedback";
import type { DeviceInfo } from "../types";

// The vault is end-to-end encryption, and all of it runs inside the bundled client: the key is
// derived from the password there, files are sealed there, and device keys are generated and
// stored there. This view only asks the client to do things and shows what it reports.

export function VaultView() {
  const { run, vault, refreshVault, profile } = useSession();
  const feedback = useFeedback();
  const [devices, setDevices] = useState<DeviceInfo[] | null>(null);
  const [deviceName, setDeviceName] = useState("");
  const [working, setWorking] = useState(false);
  const isPublic = !profile?.username;

  const act = useCallback(
    async (work: () => Promise<void>) => {
      setWorking(true);
      try {
        await work();
      } catch (e) {
        if (!(e instanceof SessionEnded)) feedback.error(String(e));
      } finally {
        setWorking(false);
      }
    },
    [feedback],
  );

  const loadDevices = useCallback(async () => {
    const result = await run({ op: "devices" });
    const event = result.events("devices")[0] as { devices: DeviceInfo[] } | undefined;
    setDevices(event?.devices ?? []);
  }, [run]);

  useEffect(() => {
    if (vault?.unlocked) void act(loadDevices);
  }, [vault?.unlocked, act, loadDevices]);

  if (isPublic) {
    return (
      <div className="panel-view">
        <header className="view-header"><h2>Vault</h2></header>
        <p className="callout">End-to-end encryption belongs to an account. Sign in with a username to use it.</p>
      </div>
    );
  }

  const enable = () =>
    act(async () => {
      const ok = await feedback.confirm({
        title: "Turn on end-to-end encryption?",
        body: (
          <>
            <p>From now on, files are encrypted on this computer before they are uploaded. The server stores only ciphertext and cannot read them.</p>
            <p><strong>There is no recovery.</strong> If you lose your password and have no enrolled device, your encrypted files cannot be opened by anyone - including the server's administrator.</p>
            <p className="hint">Files already on the server stay as they are; files uploaded from now on are sealed.</p>
          </>
        ),
        action: "Turn on encryption",
      });
      if (!ok) return;
      const result = await run({ op: "vault_init" });
      if (result.ok) feedback.ok("Your vault is set up and unlocked.");
      else feedback.error(result.message);
      await refreshVault();
    });

  const enroll = () =>
    act(async () => {
      const result = await run({ op: "enroll_device", name: deviceName.trim() || "desktop" });
      if (result.ok) feedback.ok("This device is enrolled.");
      else feedback.error(result.message);
      setDeviceName("");
      await refreshVault();
      await loadDevices();
    });

  const revoke = (device: DeviceInfo) =>
    act(async () => {
      const result = await run({ op: "revoke_device", device_id: device.device_id });
      if (result.ok) feedback.ok(result.message);
      else feedback.error(result.message);
      await refreshVault();
      await loadDevices();
    });

  return (
    <div className="panel-view">
      <header className="view-header">
        <h2>Vault</h2>
        <p className="hint">End-to-end encryption: file contents are sealed on this computer, and the server only ever holds ciphertext.</p>
      </header>

      {!vault ? (
        <p className="empty">Checking…</p>
      ) : !vault.enabled ? (
        <div className="card">
          <h3>Encryption is off for this account</h3>
          <p>Files are protected in transit {profile?.security.tls ? "by TLS" : "only if you turn on TLS"}, but the server can read them once stored.</p>
          <div className="card-actions">
            <button className="btn btn-primary" disabled={working} onClick={enable} data-testid="vault-enable">
              {working ? "Deriving your key…" : "Turn on end-to-end encryption"}
            </button>
          </div>
        </div>
      ) : !vault.unlocked ? (
        <div className="callout callout-warn">
          This account has a vault, but it is locked in this session, so encrypted files cannot be opened. Disconnect and sign in
          again with your password.
        </div>
      ) : (
        <>
          <div className="card">
            <h3><span className="badge badge-ok">Unlocked</span> Your files are encrypted before they leave this computer</h3>
            <p className="hint">
              {vault.device_enrolled
                ? `This computer is enrolled as “${vault.device_name}”, so it opens the vault with its own key instead of re-deriving it from your password.`
                : "This computer is not enrolled. Enrolling gives it its own post-quantum key to the vault, and lets you cut it off later without changing your password."}
            </p>
            {!vault.device_enrolled && (
              <div className="with-button">
                <input value={deviceName} placeholder="Name for this computer, e.g. laptop" onChange={(e) => setDeviceName(e.target.value)} />
                <button className="btn btn-primary" disabled={working} onClick={enroll}>Enroll this device</button>
              </div>
            )}
          </div>

          <div className="card">
            <h3>Enrolled devices</h3>
            {devices === null ? (
              <p className="empty">Loading…</p>
            ) : devices.length === 0 ? (
              <p className="empty">No devices are enrolled. The vault opens with your password.</p>
            ) : (
              <ul className="device-list">
                {devices.map((d) => (
                  <li key={d.device_id}>
                    <div>
                      <strong>{d.device_name}</strong> {d.this_device && <span className="badge">this device</span>}
                      <div className="hint mono">{d.algorithm} · {d.device_id}</div>
                    </div>
                    <button className="btn btn-danger-quiet" disabled={working} onClick={() => revoke(d)}>Revoke</button>
                  </li>
                ))}
              </ul>
            )}
            <p className="hint">
              Revoking stops a device from unlocking the vault from now on. It does not change the vault key, so a device that
              was compromised while enrolled should be treated as having seen it.
            </p>
          </div>
        </>
      )}
    </div>
  );
}
