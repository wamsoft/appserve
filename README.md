# appserve

ブラウザを UI とするローカルアプリのための、ミニマムな C++ フレームワーク。

- ローカル HTTP サーバとして起動し、自分にブラウザを接続させる
- UI は HTML + JavaScript、ロジックは C++ 側の API
- 派生アプリは本体を CMake で取り込み、独自 API + 独自 UI を足すだけ
- AI コーディング前提の REPL / エージェント駆動機構を標準搭載

依存は **標準ライブラリ + OS ソケットのみ**。vcpkg も FetchContent も不要で、
CMake と C++17 コンパイラだけでビルドできる。

設計の詳細は [docs/DESIGN.md](docs/DESIGN.md)、エージェント駆動は
[docs/REPL.md](docs/REPL.md) を参照。

---

## ビルド

```bash
cmake --preset windows          # MSVC (Developer Command Prompt から)
cmake --build --preset windows  # Debug
cmake --build --preset windows-rel
```

Linux / macOS は `--preset linux` (または素の `cmake -B build`)。

## 起動

```bash
appserve                    # ブラウザが --app モードで開く
appserve D:/work            # 起動時に開くパスを指定
appserve --port=8899 --no-browser
appserve --repl             # stdin 対話 REPL つき
```

主なオプション (全ては `appserve --help`):

| | |
|---|---|
| `--host=ADDR` `--port=N` | bind 先。既定は `127.0.0.1` + ポート自動割当 |
| `--web-root=PATH` | UI のディレクトリ / zip を明示指定 |
| `--browser=app\|default\|none` | ブラウザ起動方式 (既定 `app`) |
| `--idle-timeout=SEC` | 接続 0 が続いたら終了 (既定 10 秒、`0` で無効) |
| `--repl` / `--replfile=DIR` | REPL チャネル |
| `--root=DIR` / `--allow-write` | ファイル API の許可範囲 |
| `--no-token` | トークン検証を切る (開発用) |

ブラウザは Edge → Chrome を `--app=<url>` で試し、いずれも起動できなければ
OS 既定のブラウザにフォールバックする。ブラウザを閉じるとセッションが切れ、
`--idle-timeout` 秒後にプロセスが自動終了する (プロセスが残らない)。

## UI アセットの解決順

```
1. --web-root=PATH
2. カレントディレクトリの web/       ← 開発中はこれが勝つ
3. 実行ファイル隣の web/
4. 実行ファイル隣の <exe名>.zip / web.zip
5. 実行ファイルに埋め込まれた zip     ← リリース時の単一 exe
```

開発中は `web/` を編集してブラウザをリロードするだけで反映される。
リリース時は `appserve_embed_web()` が `web/` を zip 化して exe に埋め込むので、
exe 1 つで配布できる。解決結果は REPL の `.web` で確認できる。

---

## 派生アプリの作り方

```cpp
// psdapp/psd_module.cpp
#include <appserve/appserve.h>

class PsdModule : public appserve::IModule {
public:
	const char* name() const override { return "psd"; }

	void registerApi(appserve::ApiRegistry& reg) override {
		// Affinity::Main = メインスレッドで直列実行 (既定)。
		// スレッドセーフでない C++ ライブラリをそのまま API 化できる。
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
	app.addModule(appserve::makeFsModule());        // 標準のファイル API を再利用
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

UI 側は `web/lib/appserve.js` (本体からコピー) を使う:

```js
import { app } from './lib/appserve.js';

await app.ready();
const doc = await app.post('/api/psd/open', { path: file });

app.command('reload', () => refresh());          // REPL の .b call reload から呼べる
app.exposeState(() => ({ file, layers: doc.layers.length }));  // .b state から見える
app.on('progress', d => bar.value = d.pct);      // サーバの broadcast("progress", …)
```

`appserve.js` がトークン付与・セッション管理・ロングポーリング・SSE・
離脱通知をすべて内包するので、アプリ側は `get` / `post` / `command` /
`exposeState` だけ意識すればよい。UI フレームワークは不問。

動的プラグイン (DLL) で API を足す場合は
[`include/appserve/plugin_abi.h`](include/appserve/plugin_abi.h) を参照。

---

## セキュリティ

localhost bind だけでは、同一マシンの他プロセスや悪意ある Web ページ
(CSRF / DNS rebinding) からローカルファイル API を叩けてしまう。そのため:

- 起動時に 128bit のトークンを生成し、ブラウザには `?t=<token>` で渡す
- `appserve.js` が `sessionStorage` へ退避して URL からは消し、以降の
  全リクエストに `X-App-Token` を付ける
- `/_app/*` と `/api/*` はトークン + `Origin` を検証する (静的 UI は素通し)
- 既定 bind は `127.0.0.1`。それ以外を指定すると起動ログに警告が出る

## ライセンス

MIT
