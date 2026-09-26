import { describe, expect, it } from "vitest";
import { baseName, fmToRemote, isWithin, joinLocal, joinRemote, parentRemote, remoteToFm } from "./paths";
import { isoTime, mergeListing } from "./tree";
import { parsePosture } from "./posture";
import { formatBytes, percent } from "./format";
import type { FmFile } from "../types";
import { isQuestion } from "../session";

describe("paths", () => {
  it("maps the file manager's root to the remote root and back", () => {
    expect(fmToRemote("")).toBe("/");
    expect(remoteToFm("/")).toBe("");
    expect(remoteToFm("/Docs/")).toBe("/Docs");
    expect(fmToRemote("/Docs/My File.txt")).toBe("/Docs/My File.txt");
  });

  it("joins and splits remote paths", () => {
    expect(joinRemote("/", "a b")).toBe("/a b");
    expect(joinRemote("/x/", "y")).toBe("/x/y");
    expect(parentRemote("/x/y")).toBe("/x");
    expect(parentRemote("/x")).toBe("/");
    expect(baseName("/x/y z.txt")).toBe("y z.txt");
    expect(baseName("C:\\Users\\me\\Photos\\")).toBe("Photos");
  });

  it("joins local paths with the directory's own separator", () => {
    expect(joinLocal("/home/me", "a.txt")).toBe("/home/me/a.txt");
    expect(joinLocal("/home/me/", "a.txt")).toBe("/home/me/a.txt");
    expect(joinLocal("C:\\Users\\me\\Downloads", "a.txt")).toBe("C:\\Users\\me\\Downloads\\a.txt");
  });

  it("knows when a path is inside a folder", () => {
    expect(isWithin("/a/b", "/a")).toBe(true);
    expect(isWithin("/ab", "/a")).toBe(false);
    expect(isWithin("/a", "/a")).toBe(true);
  });
});

describe("mergeListing", () => {
  const dir = (path: string): FmFile => ({ name: path.split("/").pop()!, isDirectory: true, path });
  const file = (path: string): FmFile => ({ name: path.split("/").pop()!, isDirectory: false, path });

  it("builds the flat tree the file manager expects", () => {
    const files = mergeListing([], "/", [
      { name: "Docs", is_directory: true, size: 0, last_modified: 1_700_000_000 },
      { name: "a.txt", is_directory: false, size: 12, last_modified: 1_700_000_000 },
    ]);
    expect(files.map((f) => f.path).sort()).toEqual(["/Docs", "/a.txt"]);
    expect(files.find((f) => f.name === "a.txt")?.size).toBe(12);
    expect(files.find((f) => f.name === "a.txt")?.updatedAt).toBe(new Date(1_700_000_000_000).toISOString());
  });

  it("replaces one folder's children without touching its siblings' contents", () => {
    const before = [dir("/A"), dir("/B"), file("/A/old.txt"), file("/B/keep.txt")];
    const after = mergeListing(before, "/A", [{ name: "new.txt", is_directory: false, size: 1, last_modified: 0 }]);
    expect(after.map((f) => f.path).sort()).toEqual(["/A", "/A/new.txt", "/B", "/B/keep.txt"]);
  });

  it("never lets a bad timestamp break rendering", () => {
    // What servers before the file-clock fix reported for every file.
    expect(isoTime(18_446_744_069_414_584_000)).toBeUndefined();
    expect(isoTime(0)).toBeUndefined();
    const files = mergeListing([], "/", [{ name: "x", is_directory: false, size: 1, last_modified: 18_446_744_069_414_584_000 }]);
    expect(files[0].updatedAt).toBeUndefined();
  });

  it("drops everything cached under a folder that disappeared", () => {
    const before = [dir("/A"), dir("/A/sub"), file("/A/sub/deep.txt"), dir("/B")];
    const after = mergeListing(before, "/", [{ name: "B", is_directory: true, size: 0, last_modified: 0 }]);
    expect(after.map((f) => f.path)).toEqual(["/B"]);
  });
});

describe("parsePosture", () => {
  it("reads the negotiated TLS parameters and spots a hybrid group", () => {
    const p = parsePosture("Secure connection: TLSv1.3, cipher TLS_AES_256_GCM_SHA384, group X25519MLKEM768");
    expect(p).toEqual({
      encrypted: true,
      protocol: "TLSv1.3",
      cipher: "TLS_AES_256_GCM_SHA384",
      group: "X25519MLKEM768",
      postQuantum: true,
    });
  });

  it("reports a classical group as not post-quantum", () => {
    expect(parsePosture("Secure connection: TLSv1.3, cipher TLS_AES_128_GCM_SHA256, group X25519")?.postQuantum).toBe(false);
  });

  it("ignores every other message", () => {
    expect(parsePosture("Authentication successful.")).toBeNull();
  });
});

describe("format", () => {
  it("formats sizes and percentages", () => {
    expect(formatBytes(0)).toBe("0 B");
    expect(formatBytes(1536)).toBe("1.5 KB");
    expect(formatBytes(700 * 1024 * 1024)).toBe("700 MB");
    expect(percent(1, 3)).toBe(33);
    expect(percent(5, 0)).toBe(0);
  });
});

describe("isQuestion", () => {
  it("tells the server's questions from its refusals", () => {
    expect(isQuestion("User does not exist. Do you want to register? (Y/n)")).toBe(true);
    expect(isQuestion("For registration, please provide a password.")).toBe(true);
    expect(isQuestion("Invalid password. Try again.")).toBe(false);
    expect(isQuestion("Too many failed attempts. Try again in 30 second(s).")).toBe(false);
  });
});
