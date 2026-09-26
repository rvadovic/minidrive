# Debian 13 (trixie) rather than Ubuntu 24.04, for OpenSSL.
#
# The TLS transport's headline feature is the hybrid post-quantum group X25519MLKEM768, which needs
# OpenSSL 3.5 or newer (it ships ML-KEM natively - no liboqs). Ubuntu 24.04 is pinned to OpenSSL
# 3.0.x for the life of the LTS with no backport, so on that base the hybrid path could not be
# built or tested at all - only its classical fallback. Debian 13 ships OpenSSL 3.5.x from apt, and
# is also the deployment target this server assumes elsewhere (see the storage-tiering notes on
# mdadm/btrfs/ZFS).
#
# Note the tag is `debian-13`, not `1-debian-13`: the major-pinned variant is not published for 13.
FROM mcr.microsoft.com/devcontainers/cpp:debian-13

# libssl-dev is what MINIDRIVE_ENABLE_TLS finds at configure time; without it the build silently
# drops to rung 0 only. The openssl CLI is used by lab/gen-certs.sh and by the tls test suite.
RUN apt-get update && export DEBIAN_FRONTEND=noninteractive \
    && apt-get -y install --no-install-recommends cmake curl ca-certificates gnupg libssl-dev openssl

# NodeSource publishes a single distribution-agnostic suite ("nodistro"), so this is unaffected by
# the base image change.
RUN curl -fsSL https://deb.nodesource.com/setup_lts.x | bash - \
    && apt-get -y install --no-install-recommends nodejs \
    && npm install -g @anthropic-ai/claude-code \
    && rm -rf /var/lib/apt/lists/*

# The desktop app (gui/): Tauri's Linux prerequisites, plus Xvfb and WebKitWebDriver so its
# end-to-end test can drive the real window headlessly (gui/tests/e2e.py, docs/gui.md).
RUN apt-get update && export DEBIAN_FRONTEND=noninteractive \
    && apt-get -y install --no-install-recommends libwebkit2gtk-4.1-dev libxdo-dev \
       libayatana-appindicator3-dev librsvg2-dev file xvfb webkit2gtk-driver \
    && rm -rf /var/lib/apt/lists/*

# Rust for the app's core, installed for the container user rather than root, and tauri-driver
# for the end-to-end test.
USER vscode
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal --component clippy \
    && ~/.cargo/bin/cargo install tauri-driver --locked
USER root
