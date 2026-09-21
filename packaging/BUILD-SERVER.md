# Building the MiniDrive server on Debian 13

This source release has everything the **server** needs: its code, the shared library, the CMake
build, `lab/gen-certs.sh`, and the dev container (`.devcontainer/`). The client is not in it. Client
binaries are separate release downloads.

The server targets **Debian 13 (trixie)**. It links the system OpenSSL dynamically on purpose, so
`apt` security updates reach its TLS stack without a rebuild. Debian 13 ships OpenSSL 3.5, which
adds the hybrid post-quantum group `X25519MLKEM768`. Older OpenSSL versions still build, but fall
back to classical key exchange with a warning.

To skip building, install the prebuilt `minidrive-server-<version>-debian13-amd64.deb` from the
same release with `sudo apt install ./minidrive-server-*.deb`.

## 1. Install the build dependencies

```sh
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
    build-essential cmake git ca-certificates libssl-dev openssl
```

`git` is required: the first configure fetches Asio, nlohmann/json, spdlog and libsodium with
CMake's `FetchContent`. `libssl-dev` enables the TLS transport. Without it the server still
builds, but only for plaintext rung 0.

The dev container gives you the same environment: open this directory in VS Code with the Dev
Containers extension, or build `.devcontainer/Dockerfile` yourself.

## 2. Build

Run these from the unpacked source directory:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

The configure output should include `OpenSSL 3.5.x found - TLS transport enabled`. The server
binary is `build/server/server`.

## 3. Install

```sh
sudo cmake --install build --component server
```

This installs `/usr/local/bin/server`, the documentation under `/usr/local/share/doc/minidrive-server/`,
and the certificate generator as `/usr/local/share/minidrive/gen-certs.sh`.

## 4. Run it

To run over TLS, generate a CA and a server certificate once. Give the script an output directory,
because the default location next to the script is not writable after a system install:

```text
SERVER_SANS="DNS:nas.example.com,IP:10.0.0.5" \
    /usr/local/share/minidrive/gen-certs.sh /etc/minidrive/certs

server --port 9000 --root /srv/minidrive --rung 3.5 \
    --tls-cert /etc/minidrive/certs/server.crt --tls-key /etc/minidrive/certs/server.key
```

Give clients `ca.crt` (for `--ca-file`) or the `server.pin` value (for `--pin`). See
`docs/tls.md` for the full TLS setup and `README.md` for storage tiers and the other options.

## Tests

`tests/integration/` holds the project's black-box suites. They start a real server **and** a real
client, and the client is not part of this release, so run them from a full checkout of the
repository. The `ctest` step above only checks that the dependencies link.
