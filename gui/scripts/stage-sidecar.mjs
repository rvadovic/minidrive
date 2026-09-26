#!/usr/bin/env node
// Copies a built MiniDrive client to where Tauri's `externalBin` expects it:
//   src-tauri/binaries/minidrive-client-<target-triple>[.exe]
// Tauri strips the triple when it bundles, so at runtime the client sits beside the GUI
// executable as plain `minidrive-client`.
//
// Usage:
//   node scripts/stage-sidecar.mjs [path/to/client] [--target <triple>]
// Defaults: $MINIDRIVE_BUILD_DIR/client/client, else ../build/client/client; the host triple
// reported by `rustc -vV`.

import { execFileSync } from "node:child_process";
import { chmodSync, copyFileSync, existsSync, mkdirSync, statSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const gui = resolve(here, "..");
const repo = resolve(gui, "..");

const args = process.argv.slice(2);
let target = process.env.TAURI_ENV_TARGET_TRIPLE || "";
let source = "";
for (let i = 0; i < args.length; i++) {
  if (args[i] === "--target") target = args[++i];
  else source = args[i];
}

if (!target) {
  const info = execFileSync("rustc", ["-vV"], { encoding: "utf8" });
  target = /^host:\s*(\S+)/m.exec(info)?.[1] ?? "";
  if (!target) throw new Error("could not determine the Rust host triple from `rustc -vV`");
}

const exe = target.includes("windows") ? ".exe" : "";
if (!source) {
  const buildDir = process.env.MINIDRIVE_BUILD_DIR ? resolve(process.env.MINIDRIVE_BUILD_DIR) : join(repo, "build");
  // Multi-config generators (Visual Studio) put it one level deeper.
  const candidates = [join(buildDir, "client", `client${exe}`), join(buildDir, "client", "Release", `client${exe}`)];
  source = candidates.find((c) => existsSync(c)) ?? candidates[0];
}
source = resolve(source);
if (!existsSync(source) || !statSync(source).isFile()) {
  console.error(`No MiniDrive client at ${source}. Build it first (cmake --build build), or pass its path.`);
  process.exit(1);
}

const destDir = join(gui, "src-tauri", "binaries");
mkdirSync(destDir, { recursive: true });
const dest = join(destDir, `minidrive-client-${target}${exe}`);
copyFileSync(source, dest);
if (!exe) chmodSync(dest, 0o755);
console.log(`staged ${source}\n    -> ${dest}`);
