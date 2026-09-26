// Shapes crossing the Tauri boundary. The Rust side is authoritative (src-tauri/src), and what the
// client itself sends is documented in docs/ipc.md.

export type PromptKind = "command" | "password" | "confirm";

export interface Prompt {
  kind: PromptKind;
  text: string;
}

export type Frame =
  | { type: "result"; ok: boolean; code: number; message: string }
  | { type: "info"; text: string }
  | { type: "event"; name: string; data: Record<string, unknown> };

export interface Exchange {
  frames: Frame[];
  /** null exactly when the session ended during the exchange. */
  prompt: Prompt | null;
}

export interface Ending {
  exit_code: number | null;
  stderr: string[];
  last_error: string | null;
}

export interface Security {
  tls: boolean;
  caFile?: string | null;
  pin?: string | null;
  serverName?: string | null;
  requirePq: boolean;
}

export interface SyncPair {
  local: string;
  remote: string;
}

export interface Profile {
  id: string;
  name: string;
  host: string;
  port: number;
  /** Empty means public mode. */
  username: string;
  security: Security;
  syncPairs: SyncPair[];
  downloadDir?: string | null;
}

export interface LocalPath {
  path: string;
  name: string;
  exists: boolean;
  isDirectory: boolean;
  size: number;
}

/** Operations the Rust core turns into quoted command lines (src-tauri/src/ops.rs). */
export type Operation =
  | { op: "list"; path: string }
  | { op: "mkdir"; path: string }
  | { op: "rmdir"; path: string }
  | { op: "delete"; paths: string[] }
  | { op: "move"; sources: string[]; destination: string; into_directory: boolean }
  | { op: "copy"; sources: string[]; destination: string; into_directory: boolean }
  | { op: "upload"; local: string; remote: string }
  | { op: "download"; remote: string; local: string }
  | { op: "upload_dir"; local: string; remote: string }
  | { op: "download_dir"; remote: string; local: string }
  | { op: "sync"; local: string; remote: string }
  | { op: "tiers" }
  | { op: "set_tier"; tier: string }
  | { op: "vault_status" }
  | { op: "vault_init" }
  | { op: "devices" }
  | { op: "enroll_device"; name: string }
  | { op: "revoke_device"; device_id: string };

// --- event payloads (docs/ipc.md) ---

export interface ListingEntry {
  name: string;
  is_directory: boolean;
  size: number;
  last_modified: number;
}

export interface TransferProgress {
  direction: "upload" | "download";
  path: string;
  local_path: string;
  remote_path: string;
  transfer_id: number;
  chunks_done: number;
  chunks_total: number;
  bytes_total: number;
}

export interface BatchItem {
  label: string;
  index: number;
  total: number;
  op: string;
  remote_path: string;
  remote_path_from: string;
  local_path: string;
}

export interface BatchSummary {
  label: string;
  uploaded: number;
  downloaded: number;
  deleted: number;
  moved: number;
  copied: number;
  directories_created: number;
  skipped: number;
  failed: number;
  conflicts: string[];
}

export interface TierInfo {
  name: string;
  description: string;
  current: boolean;
}

export interface DeviceInfo {
  device_id: string;
  device_name: string;
  algorithm: string;
  this_device: boolean;
}

export interface VaultStatus {
  enabled: boolean;
  unlocked: boolean;
  device_enrolled: boolean;
  device_name: string;
}

/** An entry in the shape @cubone/react-file-manager draws. */
export interface FmFile {
  name: string;
  isDirectory: boolean;
  /** "/a/b" - the remote path; the root folder itself is "". */
  path: string;
  updatedAt?: string;
  size?: number;
}
