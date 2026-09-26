import { invoke } from "@tauri-apps/api/core";
import { listen, type UnlistenFn } from "@tauri-apps/api/event";
import type { Ending, Exchange, Frame, LocalPath, Operation, Profile } from "./types";

// Thin typed wrappers over the Rust core's commands (src-tauri/src/app.rs). No protocol text is
// built here: operations are data, and the core does the quoting.

export const api = {
  connect: (profile: Profile) => invoke<{ session: number; exchange: Exchange }>("connect", { profile }),
  cancelConnect: () => invoke<void>("cancel_connect"),
  execute: (op: Operation) => invoke<Exchange>("execute", { op }),
  respond: (answer: string) => invoke<Exchange>("respond", { answer }),
  disconnect: () => invoke<Ending | null>("disconnect"),
  loadProfiles: () => invoke<Profile[]>("load_profiles"),
  saveProfiles: (profiles: Profile[]) => invoke<void>("save_profiles", { profiles }),
  inspectLocalPaths: (paths: string[]) => invoke<LocalPath[]>("inspect_local_paths", { paths }),
  defaultDownloadDir: () => invoke<string | null>("default_download_dir"),
};

export function onFrame(handler: (session: number, frame: Frame) => void): Promise<UnlistenFn> {
  return listen<{ session: number; frame: Frame }>("minidrive://frame", (e) => handler(e.payload.session, e.payload.frame));
}

export function onEnded(handler: (session: number, ending: Ending) => void): Promise<UnlistenFn> {
  return listen<{ session: number; ending: Ending }>("minidrive://ended", (e) => handler(e.payload.session, e.payload.ending));
}
