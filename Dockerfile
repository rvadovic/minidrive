# MiniDrive server image.
#
# Two stages: a full Debian 13 toolchain builds the server, and the runtime stage receives nothing
# but the stripped binary. Debian 13 on both sides, for the same reason as .devcontainer/: it ships
# OpenSSL 3.5, which gives the TLS transport the hybrid post-quantum group X25519MLKEM768.
#
# libssl stays dynamic on purpose (see "Release Artifacts" in the docs): rebuilding this image on a
# fresh debian:13-slim picks up Debian's OpenSSL security fixes without touching MiniDrive's code.
# libstdc++/libgcc are linked statically (MINIDRIVE_STATIC_RUNTIME) and libsodium is built in, so
# libc and libssl are the only shared libraries the binary needs.
#
#   docker build -t minidrive-server .
#   docker run -p 9000:9000 -v minidrive-data:/srv/minidrive minidrive-server
#
# Over TLS (certificates from lab/gen-certs.sh, see docs/tls.md):
#   docker run -p 9000:9000 -v minidrive-data:/srv/minidrive \
#       -v /etc/minidrive/certs:/certs:ro minidrive-server \
#       --port 9000 --root /srv/minidrive --rung 3.5 \
#       --tls-cert /certs/server.crt --tls-key /certs/server.key

# ---- build -------------------------------------------------------------------------------------
FROM debian:13 AS build

# git is needed by CMake's FetchContent (Asio, nlohmann/json, spdlog, libsodium).
RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
       build-essential cmake git ca-certificates libssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
# Only what the server build reads. The client, GUI and tests are not configured at all.
COPY CMakeLists.txt README.md ./
COPY cmake/ cmake/
COPY shared/ shared/
COPY server/ server/
COPY docs/ docs/
COPY lab/ lab/

# No .git in the build context, so the binary reports the plain project() version.
RUN cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DMINIDRIVE_BUILD_CLIENT=OFF \
        -DMINIDRIVE_BUILD_TESTS=OFF \
    && cmake --build build -j"$(nproc)" \
    && strip build/server/server \
    # Fail the build here rather than ship a server that silently came out rung-0-only.
    && ldd build/server/server | grep -q libssl

# ---- runtime -----------------------------------------------------------------------------------
FROM debian:13-slim

# libssl3t64 is the server's only shared dependency beyond libc (apt brings in the zlib/zstd that
# libcrypto itself links).
RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends libssl3t64 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --home-dir /srv/minidrive --shell /usr/sbin/nologin minidrive \
    && install -d -o minidrive -g minidrive -m 0750 /srv/minidrive

COPY --from=build /src/build/server/server /usr/local/bin/minidrive-server

USER minidrive
VOLUME ["/srv/minidrive"]
EXPOSE 9000

# The server shuts down cleanly on SIGTERM (Docker's default stop signal), persisting partial
# transfers so clients can resume them after a restart.
ENTRYPOINT ["/usr/local/bin/minidrive-server"]
CMD ["--port", "9000", "--root", "/srv/minidrive"]
