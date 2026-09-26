import { useCallback, useEffect, useState } from "react";
import { useSession, SessionEnded } from "../session";
import { useFeedback } from "./Feedback";
import type { TierInfo } from "../types";

// Storage tiers are the server's named media (fast SSD, archive HDD, ...). Changing tier moves
// all of this account's data; the server asks for confirmation itself, and that question arrives
// through the ordinary question dialog.

export function StorageView() {
  const { run, profile } = useSession();
  const feedback = useFeedback();
  const [tiers, setTiers] = useState<TierInfo[] | null>(null);
  const [moving, setMoving] = useState<string | null>(null);
  const isPublic = !profile?.username;

  const load = useCallback(async () => {
    try {
      const result = await run({ op: "tiers" });
      const event = result.events("tiers")[0] as { tiers: TierInfo[] } | undefined;
      if (!result.ok) feedback.error(result.message);
      setTiers(event?.tiers ?? []);
    } catch (e) {
      if (!(e instanceof SessionEnded)) feedback.error(String(e));
    }
  }, [run, feedback]);

  useEffect(() => {
    void load();
  }, [load]);

  const moveTo = async (tier: string) => {
    setMoving(tier);
    try {
      const result = await run({ op: "set_tier", tier });
      if (result.ok) feedback.ok(result.message);
      else feedback.error(result.message);
      await load();
    } catch (e) {
      if (!(e instanceof SessionEnded)) feedback.error(String(e));
    } finally {
      setMoving(null);
    }
  };

  return (
    <div className="panel-view">
      <header className="view-header">
        <h2>Storage</h2>
        <p className="hint">
          The media this server offers. Your files live on one of them; moving to another copies everything, checks the
          copy, and only then removes the original.
        </p>
      </header>
      {isPublic && <p className="callout">The public area is not tiered. Sign in with an account to choose where your files live.</p>}
      {tiers === null ? (
        <p className="empty">Loading…</p>
      ) : tiers.length === 0 ? (
        <p className="empty">This server does not advertise any storage tiers.</p>
      ) : (
        <ul className="tier-list">
          {tiers.map((tier) => (
            <li key={tier.name} className={`card tier ${tier.current ? "current" : ""}`}>
              <div>
                <div className="tier-name">
                  {tier.name} {tier.current && <span className="badge badge-ok">current</span>}
                </div>
                {tier.description && <div className="hint">{tier.description}</div>}
              </div>
              {!tier.current && !isPublic && (
                <button className="btn" disabled={moving !== null} onClick={() => moveTo(tier.name)}>
                  {moving === tier.name ? "Moving…" : "Move my files here"}
                </button>
              )}
            </li>
          ))}
        </ul>
      )}
    </div>
  );
}
