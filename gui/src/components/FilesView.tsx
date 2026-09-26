import { useCallback, useEffect, useRef, useState } from "react";
import { FileManager, type CuboneFile } from "@cubone/react-file-manager";
import "@cubone/react-file-manager/dist/style.css";
import { getCurrentWebview } from "@tauri-apps/api/webview";
import { open } from "@tauri-apps/plugin-dialog";
import { api } from "../api";
import { useSession, SessionEnded, type OpResult } from "../session";
import { useFeedback } from "./Feedback";
import { mergeListing } from "../lib/tree";
import { baseName, fmToRemote, isWithin, joinLocal, joinRemote, parentRemote } from "../lib/paths";
import type { FmFile, ListingEntry } from "../types";

// The file manager component draws the tree, breadcrumbs, context menus and selection. Every
// action it reports becomes one MiniDrive operation; after it, the affected folders are listed
// again, so what is shown is always what the server says rather than an optimistic guess.

export function FilesView() {
  const { run, profile } = useSession();
  const feedback = useFeedback();
  const [files, setFiles] = useState<FmFile[]>([]);
  const [loading, setLoading] = useState(true);
  const [dropping, setDropping] = useState(false);
  const cwd = useRef("/"); // remote path of the folder being shown

  const report = useCallback(
    (result: OpResult, success?: string) => {
      if (!result.ok) feedback.error(result.message || `Failed (${result.code})`);
      else if (success) feedback.ok(success);
      return result.ok;
    },
    [feedback],
  );

  // Session-level failures (disconnected mid-way) are reported by the session itself.
  const guard = useCallback(
    async (work: () => Promise<void>) => {
      try {
        await work();
      } catch (e) {
        if (!(e instanceof SessionEnded)) feedback.error(String(e));
      }
    },
    [feedback],
  );

  const list = useCallback(
    async (dir: string) => {
      const result = await run({ op: "list", path: dir });
      const listing = result.events("listing")[0] as { entries: ListingEntry[] } | undefined;
      if (!result.ok || !listing) {
        report(result);
        return;
      }
      setFiles((prev) => mergeListing(prev, dir, listing.entries));
    },
    [run, report],
  );

  const refresh = useCallback(
    async (...dirs: string[]) => {
      for (const dir of [...new Set(dirs)]) await list(dir);
    },
    [list],
  );

  const started = useRef(false);
  useEffect(() => {
    if (started.current) return; // once per session: this view stays mounted while connected
    started.current = true;
    guard(() => list("/")).finally(() => setLoading(false));
  }, [guard, list]);

  const uploadPaths = useCallback(
    async (paths: string[], into: string) => {
      const items = await api.inspectLocalPaths(paths);
      let uploaded = 0;
      for (const item of items) {
        const remote = joinRemote(into, item.name);
        if (!item.exists) {
          feedback.error(`${item.name} no longer exists.`);
          continue;
        }
        if (item.isDirectory) {
          const result = await run({ op: "upload_dir", local: item.path, remote });
          if (report(result)) uploaded++;
          continue;
        }
        if (item.size === 0) {
          feedback.error(`${item.name} is empty, and MiniDrive cannot store empty files yet.`);
          continue;
        }
        let result = await run({ op: "upload", local: item.path, remote });
        if (!result.ok && result.code === 412 && /exists/i.test(result.message)) {
          const replace = await feedback.confirm({
            title: "Replace file?",
            body: (
              <>
                <strong>{item.name}</strong> already exists in <code>{into}</code>. Replace it with the file from this computer?
              </>
            ),
            action: "Replace",
            danger: true,
          });
          if (!replace) continue;
          const removed = await run({ op: "delete", paths: [remote] });
          if (!report(removed)) continue;
          result = await run({ op: "upload", local: item.path, remote });
        }
        if (report(result)) uploaded++;
      }
      if (uploaded > 0) feedback.ok(`Uploaded ${uploaded} item${uploaded === 1 ? "" : "s"} to ${into}`);
      await refresh(into);
    },
    [run, report, refresh, feedback],
  );

  // Files dropped from the desktop arrive as real paths through Tauri, which is what the client
  // needs - a browser File object has no path to upload from.
  useEffect(() => {
    const unlisten = getCurrentWebview().onDragDropEvent((event) => {
      const payload = event.payload;
      if (payload.type === "enter" || payload.type === "over") setDropping(true);
      else if (payload.type === "leave") setDropping(false);
      else if (payload.type === "drop") {
        setDropping(false);
        void guard(() => uploadPaths(payload.paths, cwd.current));
      }
    });
    return () => void unlisten.then((f) => f());
  }, [guard, uploadPaths]);

  const chooseUpload = (directory: boolean) =>
    guard(async () => {
      const chosen = await open({ multiple: !directory, directory });
      const paths = chosen === null ? [] : Array.isArray(chosen) ? chosen : [chosen];
      if (paths.length) await uploadPaths(paths, cwd.current);
    });

  const download = (targets: CuboneFile[]) =>
    guard(async () => {
      if (!targets.length) return;
      const defaultPath = profile?.downloadDir || (await api.defaultDownloadDir()) || undefined;
      const dir = await open({ directory: true, multiple: false, defaultPath, title: "Download to…" });
      if (typeof dir !== "string") return;
      let done = 0;
      for (const target of targets) {
        const remote = fmToRemote(target.path);
        const local = joinLocal(dir, target.name);
        const result = await run(target.isDirectory ? { op: "download_dir", remote, local } : { op: "download", remote, local });
        if (report(result)) done++;
      }
      if (done) feedback.ok(`Downloaded ${done} item${done === 1 ? "" : "s"} to ${dir}`);
    });

  return (
    <div className="files-view">
      <div className="action-bar">
        <button className="btn btn-primary" onClick={() => chooseUpload(false)} data-testid="upload-files">
          Upload files
        </button>
        <button className="btn" onClick={() => chooseUpload(true)}>
          Upload folder
        </button>
        <span className="hint">…or drop files and folders anywhere on this window.</span>
      </div>
      <div className="fm-host" data-testid="file-manager">
        <FileManager
          files={files}
          isLoading={loading}
          layout="list"
          height="100%"
          enableFilePreview={false}
          primaryColor="#3f5bd6"
          fontFamily="inherit"
          permissions={{ upload: false }}
          onFolderChange={(path) =>
            guard(async () => {
              cwd.current = fmToRemote(path);
              await list(cwd.current);
            })
          }
          onRefresh={() => guard(() => list(cwd.current))}
          onCreateFolder={(name, parent) =>
            guard(async () => {
              const dir = parent ? fmToRemote(parent.path) : cwd.current;
              report(await run({ op: "mkdir", path: joinRemote(dir, name) }));
              await refresh(dir);
            })
          }
          onRename={(file, newName) =>
            guard(async () => {
              const from = fmToRemote(file.path);
              const dir = parentRemote(from);
              report(await run({ op: "move", sources: [from], destination: joinRemote(dir, newName), into_directory: false }));
              await refresh(dir);
            })
          }
          onDelete={(targets) =>
            guard(async () => {
              const dirs = targets.filter((t) => t.isDirectory).map((t) => fmToRemote(t.path));
              const plain = targets.filter((t) => !t.isDirectory).map((t) => fmToRemote(t.path));
              for (const dir of dirs) report(await run({ op: "rmdir", path: dir }));
              if (plain.length) report(await run({ op: "delete", paths: plain }));
              await refresh(...targets.map((t) => parentRemote(fmToRemote(t.path))));
            })
          }
          onPaste={(sources, destination, type) =>
            guard(async () => {
              const dest = destination ? fmToRemote(destination.path) : cwd.current;
              const paths = sources.map((s) => fmToRemote(s.path)).filter((p) => parentRemote(p) !== dest || type === "copy");
              if (sources.some((s) => s.isDirectory && isWithin(dest, fmToRemote(s.path)))) {
                feedback.error("A folder cannot be moved or copied into itself.");
                return;
              }
              if (!paths.length) return;
              const result = await run({ op: type, sources: paths, destination: dest, into_directory: true });
              report(result, paths.length > 1 ? `${type === "move" ? "Moved" : "Copied"} ${paths.length} items` : undefined);
              await refresh(dest, ...paths.map(parentRemote));
            })
          }
          onDownload={download}
          onFileOpen={(file) => {
            if (!file.isDirectory) feedback.info(`Use Download to save ${baseName(file.path)} to this computer.`);
          }}
          onError={(error) => feedback.error(error.message)}
        />
      </div>
      {dropping && (
        <div className="drop-overlay">
          <div>Drop to upload to <code>{cwd.current}</code></div>
        </div>
      )}
    </div>
  );
}
