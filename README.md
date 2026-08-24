# FENRIS

[![Ubuntu Build and Test](https://github.com/std-fenris/fenris/actions/workflows/ubuntu.yml/badge.svg)](https://github.com/std-fenris/fenris/actions/workflows/ubuntu.yml)

Fenris is a C++20 client-server file transfer and remote filesystem tool. A server
exposes a sandboxed directory tree; clients connect over TCP, perform a key exchange,
and then browse, read, and write files on the server through an encrypted, protobuf
based protocol and an interactive terminal shell.

## Features

- **Encrypted transport** — ECDH key exchange (NIST P-256) per connection, with the
  shared secret run through HKDF-SHA256 to derive a session key. Every request and
  response after the handshake is encrypted with AES-256-GCM (fresh random IV per
  message, IV prefixed to the ciphertext).
- **Remote filesystem over an interactive shell** — the client presents a `cd`-style
  prompt (`fenris:/path>`) backed by an in-memory directory tree on the server:

  | Command | Description |
  |---|---|
  | `cd <dir>` | Change the current directory |
  | `ls [dir]` | List contents of a directory |
  | `cat <file>` | Display contents of a file |
  | `upload <local_file>` | Upload a local file to the current server directory |
  | `write <file> <content>` | Create a new file with content |
  | `append <file> <content>` | Append content to an existing file |
  | `rm <file>` | Remove a file |
  | `info <file>` | Show file metadata (size, permissions, modified time) |
  | `mkdir <dir>` | Create a directory |
  | `rmdir <dir>` | Remove a directory |
  | `ping` | Check server responsiveness |
  | `help` | List available commands |
  | `exit` | Disconnect and quit |
- **Concurrent server** — each client connection is handled on its own thread against a
  shared, mutex-guarded filesystem tree, so multiple clients can browse and edit the
  same server tree concurrently.
- **LRU file cache** — the server caches recently-read file contents (bounded size,
  least-recently-used eviction) to cut down on repeated disk I/O.
- **Structured protocol** — requests and responses are Protobuf messages
  (`proto/fenris.proto`) covering pings, file CRUD, directory CRUD, and structured
  results (`FileInfo`, `DirectoryListing`), rather than an ad hoc text protocol.
- **Configurable logging** — both binaries share a common `spdlog`-backed logging setup
  with `--log-level`, `--log-file`, `--no-console-log`, and `--file-log` flags.
- **Colorized client output** — command prompts, results, and errors are colorized in
  the terminal for readability.
- **zlib-based compression utility** — `CompressionManager` (in `fenris_common`) wraps
  zlib compress/decompress for callers that need it, alongside the crypto and file
  operation utilities.

## Building

```sh
git clone --recursive <repo-url>
cd fenris
mkdir -p build && cd build
cmake -DUNIT_TESTING=ON -DCMAKE_CXX_COMPILER=g++ -DCMAKE_BUILD_TYPE=Debug ..
make -j$(nproc)
```

Requires `protobuf-compiler` and libprotobuf 3.21 installed on the system; `spdlog`,
`zlib`, `argparse`, `cryptopp`, and `googletest` are pulled in as git submodules under
`vendor/` (hence `--recursive` on clone).

This produces `build/src/server/server` and `build/src/client/client`.

## Running

Start the server:

```sh
./build/src/server/server --host 0.0.0.0 --port 5555
```

Connect with the client:

```sh
./build/src/client/client --host 127.0.0.1 --port 5555
```

Both accept `--log-level`, `--log-file`, `--no-console-log`, and `--file-log`. Running
the client with no `--host`/`--port` prompts for them interactively instead.

## Testing

```sh
cd build
ctest --verbose
```

Unit and integration tests (GoogleTest) cover the common crypto/compression/file-op/
protocol layers as well as the client and server connection and request managers.
