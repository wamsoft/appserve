# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`appserve` is a C++17 framework for local desktop apps whose UI is a browser: the process
starts a localhost HTTP server, launches Edge/Chrome in `--app` mode pointed at itself, and
exits once no browser is connected. Derived apps take it in via `add_subdirectory` /
FetchContent and add their own API modules + their own `web/`. The bundled `appserve`
executable (`app/main.cpp` + `modules/fs` + `web/`) is both the reference app and the
dogfood target.

Full design in `docs/DESIGN.md`, agent/REPL protocol in `docs/REPL.md`, packaging in
`docs/RELEASE.md` (all Japanese).

## Hard constraint: zero third-party dependencies

The only dependencies are the C++ standard library and OS sockets — no vcpkg, no
FetchContent, no vendored `third_party/`. JSON (`src/json/`), zip inflate + CRC32
(`src/webroot/zip.cpp`), UTF-8/UTF-16 conversion (`src/core/util.cpp`) and logging are all
hand-written for this reason. Do not introduce a library dependency to solve a problem in
this repo; extend the in-tree minimal implementation instead.

## Build

```bash
cmake --preset windows            # MSVC — run from a Developer Command Prompt (needs cl + ninja)
cmake --build --preset windows        # Debug
cmake --build --preset windows-rel    # Release
cmake --preset linux && cmake --build --preset linux
cmake -B build -DCMAKE_BUILD_TYPE=Release -DAPPSERVE_BUILD_APP=ON   # what CI does
```

Presets are Ninja Multi-Config, output under `build/<preset>/<config>/`.
`APPSERVE_BUILD_APP` defaults ON only for a top-level build, so pulling appserve in as a
subdirectory builds `appserve::core` alone.

## Testing

There is **no unit test suite** (`APPSERVE_BUILD_TESTS` is OFF and points at a `tests/`
directory that does not exist — adding tests means creating it and a `tests/CMakeLists.txt`).
Verification is done by running the real binary:

- CI (`.github/workflows/ci.yml`) is the canonical smoke test: start with
  `--no-browser --port=18999 --no-token --idle-timeout=0`, then `curl` `/_app/info`, `/`,
  `/lib/appserve.js`, `/api/fs/roots`, and stop with `POST /_app/repl` body `.quit`.
  Reproduce it locally verbatim when touching HTTP, webroot, or fs code.
- For anything touching the browser side, drive it through the REPL file channel (below) —
  that is the intended way to verify UI + backend together.

Linux CI is not optional decoration: it is the guard for the POSIX code paths (it already
caught an `accept()` hang on shutdown, since `close()` does not wake `accept()` on POSIX).
Anything touching `src/http/socket.cpp`, `server.cpp`, or `browser/launcher.cpp` must be
checked on both platforms.

## Driving the app as an agent

```bash
appserve --replfile=/tmp/chan --no-browser --idle-timeout=0 &
# write cmd.tmp -> rename to cmd; wait for resp; read it; DELETE it before the next command
```

Lockstep file protocol (identical to kirikiri Z's `-replfile`): the server refuses to read a
new `cmd` while an unread `resp` exists, so forgetting to delete `resp` silently shifts every
later answer by one. Response JSON is `{"ok":bool,"result":"...","error":"..."}`.
`docs/REPL.md` has a ready-made bash driver.

Server commands: `.info` `.routes` `.web` `.sessions` `.stat` `.get <path>` `.post <path> [body]`
`.push <ch> <text>` `.log` `.open` `.quit` (leading `.` optional).
Browser commands go through `.b`: `.b eval <js>` `.b dom [sel]` `.b text [sel]` `.b click <sel>`
`.b set <sel> <val>` `.b state` `.b call <name> [json]` `.b err`. **Screenshots are impossible** —
observe the UI with `.b dom` / `.b text` / `.b state`.

While a stdin or file REPL is attached, idle auto-exit is suppressed; with only the HTTP
channel it is not, so a bare `appserve --no-browser` exits by itself after
`startup-grace` + `idle-timeout`.

## Architecture

### Threads and affinity

| thread | role |
|---|---|
| main | `App::run()` loop: `TaskQueue::drain(200)`, session reaping, auto-exit decision |
| accept | listen socket accept loop (`HttpServer`) |
| conn × N | one thread per connection, kept for keep-alive; SSE and long-poll block here |
| repl | stdin reader and/or file-channel poller |

`run()` is `startServer()` + `while (serving()) pump(200)` + `stopServer()`. A host with its
own loop (SDL, a game loop) calls those three itself and drives `pump(0)` per frame; the
"main thread" is then whichever thread called `startServer()`.

Handlers registered with `Affinity::Main` (the default) are pushed onto `TaskQueue` by the
connection thread, which blocks until the main loop runs them — that is what makes it safe to
expose a non-thread-safe C++ library as an API. `Affinity::Any` runs inline on the connection
thread. `TaskQueue::submit` from the main thread executes inline to avoid self-deadlock, but a
`Main` handler that blocks the main loop stalls every other `Main` request.

### Request path (`App::Impl::dispatch`, `src/core/app.cpp`)

`/_app/*` built-ins → registered routes (longest-prefix match, `Registry`) → dir mounts →
static WebRoot. Token + `Origin` are verified for the first three only; static UI files are
served unauthenticated (the page must load before it can present a token). Handler exceptions
become 500 and are logged.

Built-ins: `/_app/info` `/hello` `/poll` `/result` `/bye` `/hb` `/repl` `/events` (log SSE)
`/sub/<ch>` (generic SSE). `/_app/*` is reserved for the framework; derived apps use `/api/...`.

### Server → browser control

`BrowserChannel::call()` parks a command on the session; the browser's pending
`/_app/poll` returns it immediately, `appserve.js` executes it and POSTs `/_app/result`,
unblocking `call()`. This is what makes `.b` commands round-trip with no polling latency.
SSE (`/_app/sub/<ch>`) is the one-way push path; long-poll is the command path. Both are held
open on connection threads and are hijacked (`Conn::hijacked`) rather than going through the
normal response writer.

### Lifecycle

A session is one browser tab, created by `/_app/hello`, kept alive by poll/SSE/hb, killed by
`/_app/bye` (sent via `pagehide`) or by `sessionTtl`. Auto-exit fires when startup grace has
passed, zero sessions are alive for `idleTimeout`, and no REPL is attached.

Shutdown is symmetric: `stopServer()` posts a `shutdown` browser command (and pauses ~150ms
for it to be delivered) so the window closes immediately; if the process dies without that,
`appserve.js` declares the server lost after `disconnectGraceMs` of unreachable polls and
closes itself. Both paths end in `Appserve._lost()`, which is also where `onDisconnected()`
listeners run. `exitOnDisconnect = false` opts a page out.

### UI asset resolution (`src/webroot/`)

`--web-root` → `./web` → `<exe>/web` → `<exe>/<name>.zip` or `web.zip` → zip linked into the
executable. **`./web` wins during development**, so edit `web/` and reload the browser — the
embedded zip only matters for releases. `.web` reports which source was picked.
`appserve_embed_web()` zips `web/` at build time and runs `cmake/bin2c.cmake` to turn it into a
linked C array (linked, not appended to the exe, so code signing survives).

### Extension points

- **Static module (preferred)**: implement `IModule` (`include/appserve/module.h`),
  register routes/REPL commands in `registerApi()`, free resources in `onShutdown()`,
  add with `App::addModule()`. `modules/fs/fs_module.cpp` is the worked example.
- **Dynamic plugin**: `plugins/*.dll|so` loaded at startup through the C ABI in
  `include/appserve/plugin_abi.h` (POD structs + function pointers only, `APPSERVE_ABI_VERSION`
  checked). Implemented but never validated against a real plugin.
- **Browser side**: `web/lib/appserve.js` is copied verbatim into derived apps; it owns the
  token, session, poll loop, SSE, and `bye` beacon. Apps only use
  `get`/`post`/`command`/`exposeState`/`on`. Any (or no) UI framework works.

### Security model

128-bit token generated at startup, handed over as `?t=…`, moved to `sessionStorage` by
`appserve.js` and stripped from the URL, then sent as `X-App-Token`. `Origin`, when present,
must match this server. `--no-token` is for CI/debug only. Non-loopback binds log a warning.
Path safety (`WebRoot::safeRelPath`) rejects `..`, backslashes and drive colons.

## Conventions

- Comments, docs and commit messages are **Japanese**; README.md is English (README_ja.md is
  the Japanese one). Commits use conventional prefixes (`fix:`, `ci:`, `docs:`) with a Japanese
  subject. Keep new code in the same style — the existing files carry substantial explanatory
  comments about *why* (especially the krkrz heritage and the platform gotchas); match that
  density rather than writing bare code.
- Indentation: **tabs** in C++ and JS, 4 spaces in CMake. Section headers are the
  `//------` rule lines seen at the top of every file.
- `include/appserve/*.h` is the public surface consumed by derived apps and plugins — keep
  implementation details in `src/**` headers (`app_impl.h`, `server.h`, `registry.h`, …), which
  derived apps never include.
- `src/http/server.h` must be included before anything else that could pull in `windows.h`
  (it takes care of `winsock2.h` ordering).
- Adding a source file means adding it to the `appserve_core` list in `CMakeLists.txt`.
- Paths are UTF-8 everywhere and converted with `std::filesystem::u8path` / the helpers in
  `src/core/util.cpp` — Japanese paths must keep working on Windows.

## Known limits (don't "fix" without cause)

Chunked request bodies return 501; zip64 is unsupported; one thread per connection is a
deliberate choice for a local tool; browser screenshots are not obtainable.
