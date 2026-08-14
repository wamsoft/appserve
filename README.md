# appserve

*[日本語版はこちら / Japanese version](README_ja.md)*

A minimal C++ framework for local applications whose UI is a browser.

- Starts as a local HTTP server and points a browser at itself
- UI in HTML + JavaScript, logic in a C++ API
- Derived apps pull this in with CMake and add their own API and UI
- Ships with a REPL and agent-driving machinery, built for AI-assisted coding

The only dependencies are **the standard library and OS sockets**. No vcpkg, no
FetchContent — CMake and a C++17 compiler are enough.

See [docs/DESIGN.md](docs/DESIGN.md) for the design and
[docs/REPL.md](docs/REPL.md) for agent driving (both in Japanese).

---

## Building

```bash
cmake --preset windows          # MSVC (from a Developer Command Prompt)
cmake --build --preset windows  # Debug
cmake --build --preset windows-rel
```

Linux and macOS use `--preset linux` (or plain `cmake -B build`).

## Running

```bash
appserve                    # a browser opens in app mode
appserve D:/work            # open at a given path
appserve --port=8899 --no-browser
appserve --repl             # with an interactive REPL on stdin
```

Main options (`appserve --help` lists them all):

| | |
|---|---|
| `--host=ADDR` `--port=N` | Bind address. Defaults to `127.0.0.1` and an automatically chosen port |
| `--web-root=PATH` | Serve the UI from a specific directory or zip |
| `--browser=app\|default\|none` | How to open the browser (default `app`) |
| `--idle-timeout=SEC` | Exit after no browser has been connected for SEC (default 10, `0` disables) |
| `--repl` / `--replfile=DIR` | REPL channels |
| `--root=DIR` / `--allow-write` | Scope of the file API |
| `--no-token` | Turn off token checks (development only) |

The browser is launched as Edge → Chrome with `--app=<url>`; if neither can be
started it falls back to the OS default browser. Closing the browser ends the
session, and the process exits `--idle-timeout` seconds later, so nothing is left
running.

## How the UI assets are resolved

```
1. --web-root=PATH
2. web/ in the current directory        ← wins during development
3. web/ next to the executable
4. <exe name>.zip / web.zip next to the executable
5. a zip embedded in the executable     ← a single-file release
```

During development you edit `web/` and reload the browser. For a release,
`appserve_embed_web()` packs `web/` into a zip inside the executable, so it ships
as one file. The REPL command `.web` reports which of these was used.

---

## Writing a derived app

```cpp
// psdapp/psd_module.cpp
#include <appserve/appserve.h>

class PsdModule : public appserve::IModule {
public:
	const char* name() const override { return "psd"; }

	void registerApi(appserve::ApiRegistry& reg) override {
		// Affinity::Main (the default) runs the handler serialised on the main
		// thread, so a C++ library that is not thread-safe can be exposed as-is.
		reg.route("/api/psd/open", appserve::Affinity::Main, [this](const auto& r) {
			doc_ = psdparse::open(r.json()["path"].asStr());
			return appserve::Response::json(describe(*doc_));
		});
		reg.replCommand("psd", "show the loaded document", [this](const std::string&) {
			return doc_ ? doc_->name() : "(nothing loaded)";
		});
	}
	void onShutdown() override { doc_.reset(); }
private:
	std::unique_ptr<psdparse::Document> doc_;
};
```

```cpp
// psdapp/main.cpp
int main(int argc, char** argv) {
	appserve::App app;
	app.options().appName = "PSD Inspector";
	if (!app.parseArgs(argc, argv)) return app.exitCode();
	app.addModule(appserve::makeFsModule());        // reuse the standard file API
	app.addModule(std::make_unique<PsdModule>());
	return app.run();
}
```

```cmake
add_subdirectory(external/appserve)
add_executable(psdapp main.cpp psd_module.cpp)
target_link_libraries(psdapp PRIVATE appserve::core psdparse)
appserve_embed_web(psdapp WEB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/web)
```

The UI side uses `web/lib/appserve.js` (copied from here):

```js
import { app } from './lib/appserve.js';

await app.ready();
const doc = await app.post('/api/psd/open', { path: file });

app.command('reload', () => refresh());          // callable as .b call reload
app.exposeState(() => ({ file, layers: doc.layers.length }));  // visible to .b state
app.on('progress', d => bar.value = d.pct);      // from broadcast("progress", …)
```

`appserve.js` takes care of the token, session handling, long polling, SSE and the
leave notification, so an app only deals with `get` / `post` / `command` /
`exposeState`. Any UI framework — or none — works.

To add API from a dynamic plugin (DLL), see
[`include/appserve/plugin_abi.h`](include/appserve/plugin_abi.h).

---

## Security

Binding to localhost alone is not enough: other processes on the same machine, and
malicious web pages (CSRF / DNS rebinding), could otherwise reach the local file
API. So:

- A 128-bit token is generated at startup and handed to the browser as `?t=<token>`
- `appserve.js` moves it into `sessionStorage`, strips it from the URL, and sends
  it as `X-App-Token` on every request
- `/_app/*` and `/api/*` verify the token and the `Origin` header (static UI files
  are served without a check)
- The default bind address is `127.0.0.1`; anything else logs a warning at startup

## Packaging and releases

`appserve_package()` builds the zip and the installer, and pushing a tag makes
GitHub Actions publish a release. See [docs/RELEASE.md](docs/RELEASE.md) (Japanese).

```bash
cmake --build --preset windows-rel
cpack --config build/windows/CPackConfig.cmake -C Release -B dist   # build locally
git tag v0.1.0 && git push origin v0.1.0                            # publish
```

## License

MIT
