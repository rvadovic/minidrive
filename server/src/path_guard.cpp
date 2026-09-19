#include "path_guard.hpp"

#include "filesystem/utils.hpp"

std::optional<std::filesystem::path>
guard_path(const std::filesystem::path& user_root,
           const std::filesystem::path& current_dir,
           const std::string& argument) {
    // An empty root would make is_subpath() vacuously true for everything, so it is rejected rather
    // than trusted. In practice setup_dir() always sets one, but this is the security boundary and
    // it should not depend on that staying true.
    if(user_root.empty()) return std::nullopt;

    std::filesystem::path resolved = fsutils::resolve_path(user_root, current_dir, argument);
    if(resolved.empty()) return std::nullopt;

    if(!fsutils::is_subpath(user_root, resolved)) return std::nullopt;

    return resolved;
}
