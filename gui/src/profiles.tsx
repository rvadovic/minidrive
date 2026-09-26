import { createContext, useCallback, useContext, useEffect, useState, type ReactNode } from "react";
import { api } from "./api";
import type { Profile } from "./types";

// Saved servers, persisted by the Rust core to <app config dir>/profiles.json. A profile holds the
// TLS posture and sync pairs for a server - never a password.

interface ProfilesApi {
  profiles: Profile[];
  loaded: boolean;
  error: string | null;
  save(profile: Profile): Promise<void>;
  remove(id: string): Promise<void>;
}

const Ctx = createContext<ProfilesApi | null>(null);

export function useProfiles(): ProfilesApi {
  const value = useContext(Ctx);
  if (!value) throw new Error("useProfiles outside ProfilesProvider");
  return value;
}

export function newProfile(): Profile {
  return {
    id: crypto.randomUUID(),
    name: "",
    host: "",
    port: 9000,
    username: "",
    security: { tls: true, caFile: null, pin: null, serverName: null, requirePq: false },
    syncPairs: [],
    downloadDir: null,
  };
}

export function ProfilesProvider({ children }: { children: ReactNode }) {
  const [profiles, setProfiles] = useState<Profile[]>([]);
  const [loaded, setLoaded] = useState(false);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    api
      .loadProfiles()
      .then(setProfiles)
      .catch((e) => setError(String(e)))
      .finally(() => setLoaded(true));
  }, []);

  const persist = useCallback(async (next: Profile[]) => {
    await api.saveProfiles(next); // throws on a validation error, leaving the list unchanged
    setProfiles(next);
  }, []);

  const save = useCallback(
    async (profile: Profile) => {
      const exists = profiles.some((p) => p.id === profile.id);
      await persist(exists ? profiles.map((p) => (p.id === profile.id ? profile : p)) : [...profiles, profile]);
    },
    [profiles, persist],
  );

  const remove = useCallback((id: string) => persist(profiles.filter((p) => p.id !== id)), [profiles, persist]);

  return <Ctx.Provider value={{ profiles, loaded, error, save, remove }}>{children}</Ctx.Provider>;
}
