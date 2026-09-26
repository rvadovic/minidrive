import type { FmFile, ListingEntry } from "../types";
import { joinRemote, remoteToFm } from "./paths";

// The file manager wants the whole known tree as one flat array, and finds a folder's children
// by `path === parent + "/" + name`. The server is asked one directory at a time, so each
// listing replaces that directory's children, and anything cached beneath a child that has
// disappeared is dropped with it.

function parentOf(fmPath: string): string {
  const cut = fmPath.lastIndexOf("/");
  return cut <= 0 ? "" : fmPath.slice(0, cut);
}

/** Unix seconds to ISO 8601, or nothing for a time that is missing or out of range. */
export function isoTime(seconds: number): string | undefined {
  if (!seconds) return undefined;
  const date = new Date(seconds * 1000);
  return Number.isNaN(date.getTime()) ? undefined : date.toISOString();
}

export function mergeListing(files: FmFile[], remoteDir: string, entries: ListingEntry[]): FmFile[] {
  const dir = remoteToFm(remoteDir);
  const kept = files.filter((f) => parentOf(f.path) !== dir);
  const fresh: FmFile[] = entries.map((e) => ({
    name: e.name,
    isDirectory: e.is_directory,
    path: remoteToFm(joinRemote(remoteDir, e.name)),
    size: e.is_directory ? undefined : e.size,
    updatedAt: isoTime(e.last_modified),
  }));
  return prune([...kept, ...fresh]);
}

/** Keeps an entry only if every folder above it is still known to exist. */
export function prune(files: FmFile[]): FmFile[] {
  const dirs = new Set(files.filter((f) => f.isDirectory).map((f) => f.path));
  return files.filter((f) => {
    for (let p = parentOf(f.path); p !== ""; p = parentOf(p)) {
      if (!dirs.has(p)) return false;
    }
    return true;
  });
}

/** Whether a folder is in the known tree (the root always is). */
export function isKnownDirectory(files: FmFile[], remoteDir: string): boolean {
  const dir = remoteToFm(remoteDir);
  return dir === "" || files.some((f) => f.isDirectory && f.path === dir);
}
