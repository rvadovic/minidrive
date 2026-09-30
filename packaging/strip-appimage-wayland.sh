#!/usr/bin/env bash
# Remove the bundled libwayland-* libraries from a Tauri AppImage, in place.
#
# Tauri's linuxdeploy step copies libwayland-client/-egl/-cursor/-server from the build host
# (Ubuntu 22.04) into the AppImage, but not libEGL, which always comes from the user's Mesa. A
# newer host Mesa driving the old bundled libwayland makes WebKitGTK abort at startup with
# "Could not create default EGL display: EGL_BAD_PARAMETER" (observed on Fedora 43). Every
# desktop that can run a GTK 3 app already has libwayland, so the host copy is always there and
# always matches the host Mesa.
#
# The image is repacked from its own contents and its own runtime (same zstd compression and
# block size linuxdeploy used), so nothing but the removed files changes.
#
# Usage: strip-appimage-wayland.sh <file.AppImage>        (needs squashfs-tools)
set -euo pipefail

img=$(realpath "$1")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

offset=$("$img" --appimage-offset)
unsquashfs -q -no-xattrs -o "$offset" -d "$work/root" "$img" >/dev/null

removed=$(find "$work/root/usr/lib" -maxdepth 1 -name 'libwayland-*.so*' -print)
if [ -z "$removed" ]; then
  echo "no bundled libwayland in $img; left unchanged"
  exit 0
fi
echo "removing from $(basename "$img"):"
echo "$removed" | sed "s|^$work/root/|  |"
rm -f $removed

head -c "$offset" "$img" > "$work/out.AppImage"
mksquashfs "$work/root" "$work/fs.squashfs" -root-owned -noappend -no-xattrs \
  -comp zstd -b 131072 -quiet -no-progress
cat "$work/fs.squashfs" >> "$work/out.AppImage"
chmod 755 "$work/out.AppImage"
mv "$work/out.AppImage" "$img"
