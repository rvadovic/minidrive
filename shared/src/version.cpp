#include "minidrive/version.hpp"

namespace minidrive {

const char* resolved_version() {
    return git_describe().empty() ? version().data() : git_describe().data();
}

} // namespace minidrive
