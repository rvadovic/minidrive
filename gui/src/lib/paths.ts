// Remote paths are always absolute and '/'-separated ("/Docs/a.txt"); the server resolves a
// leading '/' against the user's root, so the GUI never depends on the server-side CD state.
// The file manager component calls the root "" - fmToRemote/remoteToFm translate.

export function remoteToFm(remote: string): string {
  const normal = normalizeRemote(remote);
  return normal === "/" ? "" : normal;
}

export function fmToRemote(fmPath: string): string {
  return normalizeRemote(fmPath === "" ? "/" : fmPath);
}

export function normalizeRemote(path: string): string {
  const parts = path.split("/").filter((p) => p !== "" && p !== ".");
  return "/" + parts.join("/");
}

export function joinRemote(dir: string, name: string): string {
  return normalizeRemote(`${dir}/${name}`);
}

export function parentRemote(path: string): string {
  const normal = normalizeRemote(path);
  const cut = normal.lastIndexOf("/");
  return cut <= 0 ? "/" : normal.slice(0, cut);
}

export function baseName(path: string): string {
  const trimmed = path.replace(/[\\/]+$/, "");
  const cut = Math.max(trimmed.lastIndexOf("/"), trimmed.lastIndexOf("\\"));
  return cut < 0 ? trimmed : trimmed.slice(cut + 1);
}

/** Joins a local directory and a name using whichever separator the directory already uses. */
export function joinLocal(dir: string, name: string): string {
  const windows = /^[A-Za-z]:\\|^\\\\/.test(dir) || (dir.includes("\\") && !dir.includes("/"));
  const sep = windows ? "\\" : "/";
  return dir.endsWith("/") || dir.endsWith("\\") ? dir + name : dir + sep + name;
}

/** True when `path` is `ancestor` or inside it. */
export function isWithin(path: string, ancestor: string): boolean {
  const p = normalizeRemote(path);
  const a = normalizeRemote(ancestor);
  return a === "/" || p === a || p.startsWith(a + "/");
}
