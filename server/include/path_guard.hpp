#pragma once

#include <filesystem>
#include <optional>
#include <string>

// The one implementation of the path-traversal boundary.
//
// Before this existed the pattern - resolve_path() followed by is_subpath() - was copy-pasted into
// every file-touching handler, and it had drifted: most handlers asked the filesystem whether the
// path existed *before* checking whether it was inside the user's root. That leaked an existence
// oracle for the whole server filesystem, because "DELETE ../../etc/shadow" answered 403 when the
// file was there and 400 when it was not. No file ever escaped, but the error code alone let a
// caller enumerate paths it had no business knowing about.
//
// Here containment is decided first and nothing else is asked until it passes. A caller that never
// dereferences the optional cannot reintroduce that ordering.
//
// Fails closed: an unresolvable path, a path outside the root, or a canonicalization error all come
// back as std::nullopt, which every caller answers with 403 and nothing else.
std::optional<std::filesystem::path>
guard_path(const std::filesystem::path& user_root,
           const std::filesystem::path& current_dir,
           const std::string& argument);
