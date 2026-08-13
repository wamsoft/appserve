# appserve 設計書

ブラウザを UI とするローカルアプリのための、ミニマムな C++ アプリケーションフレームワーク。

- ローカル HTTP サーバとして起動し、自身にブラウザを接続させる
- UI は HTML + JavaScript、ロジックは C++ 側の API
- 派生アプリは本体を CMake で取り込み、独自 API + 独自 UI を足すだけ
- AI コーディング前提の REPL / エージェント駆動機構を標準搭載

母体は吉里吉里Z の `common/utils/ReplWebServer.cpp`（HTTP + SSE + 動的ハンドラ +
静的配信 + ブラウザ起動）。そこから TJS 依存を全て剥がし、pure C++17 に再構成する。

---

## 1. スコープと非スコープ

### やること
| | |
|---|---|
| ローカル HTTP/1.1 サーバ | 静的配信 + JSON API + SSE + ロングポーリング |
| UI 供給 | 開発時は `web/` ディレクトリ、リリース時は exe 埋め込み zip |
| ブラウザ起動 | Edge/Chrome `--app` → 失敗時は既定ブラウザ |
| ライフサイクル | 接続が無くなったら自動終了 |
| 拡張機構 | 静的モジュール（推奨）+ 動的プラグイン DLL（C ABI） |
| REPL | stdin / ファイルチャネル / HTTP。サーバ状態確認 **+ ブラウザ側の遠隔操作** |
| 標準モジュール | ローカルファイルアクセス API + それを使う UI |

### やらないこと
- 認証つきマルチユーザ、TLS、リバースプロキシ用途（ローカル専用ツール）
- スクリプトエンジンの内蔵（REPL はコマンド式。JS 評価はブラウザ側で行う）
- HTTP/2、WebSocket（SSE + ロングポーリングで足りる。必要になったら追加）

---

## 2. 全体構成

```
                          ┌──────────────── appserve (1 プロセス) ────────────────┐
                          │                                                        │
  ┌──────────┐   HTTP     │  ┌──────────┐   dispatch    ┌────────────────────┐   │
  │ Browser  │◄──────────►│  │ HttpSrv  │──────────────►│ Router             │   │
  │  (UI)    │            │  │ accept   │               │  /            静的  │   │
  │          │            │  │ thread   │               │  /_app/*      内蔵  │   │
  │ appserve │◄─ poll ────│  │  + 1 th  │               │  /api/fs/*    標準  │   │
  │   .js    │── result ─►│  │  /conn   │               │  /api/xxx/*   派生  │   │
  └──────────┘            │  └────┬─────┘               └─────────┬──────────┘   │
                          │       │                               │              │
                          │       │  Affinity::Main のハンドラ    │              │
                          │       └──────► TaskQueue ─────────────┘              │
                          │                    │                                  │
                          │            ┌───────▼────────┐   ┌──────────────┐     │
  ┌──────────┐  cmd/resp  │            │ Main loop      │   │ SessionMgr   │     │
  │  Agent   │◄──────────►│  ReplFile  │  drain + idle  │◄──│ 生存監視     │     │
  │ (AI/CI)  │            │  Channel   │  監視          │   │ 自動終了     │     │
  └──────────┘            │            └───────┬────────┘   └──────────────┘     │
                          │                    │                                  │
  ┌──────────┐  stdin     │  ReplStdin  ───────┤                                  │
  │  開発者  │◄──────────►│                    ▼                                  │
  └──────────┘            │            BrowserChannel  ── push cmd ──► poll 応答  │
                          └────────────────────────────────────────────────────────┘
```

スレッド構成（krkrz の thread-per-connection を踏襲）:

| スレッド | 役割 |
|---|---|
| main | `App::run()`。タスクキュー drain / アイドル監視 / シャットダウン |
| accept | listen ソケットの `accept` ループ |
| conn × N | 接続 1 本 = 1 スレッド。keep-alive のあいだ保持。SSE/ロングポーリングもここ |
| repl（任意）| stdin 読取 or ファイルチャネル監視 |

`Affinity::Main` のハンドラは `TaskQueue::submit()` で main へ運び、完了を待って
conn スレッドが応答を書く（krkrz `ReplMainQueue::SubmitTask` と同じモデル）。
これにより psdparse のようなスレッドセーフでない C++ ライブラリを安全に API 化できる。

---

## 3. ディレクトリ構成

```
appserve/
├─ CMakeLists.txt              appserve::core / appserve::app
├─ CMakePresets.json
├─ cmake/
│  ├─ AppserveEmbedWeb.cmake   appserve_embed_web() : web/ → zip → C 配列 → リンク
│  └─ bin2c.cmake              スクリプトモード用ヘルパ（外部ツール不要）
├─ include/appserve/           ← 派生アプリ / プラグインが include する公開ヘッダ
│  ├─ appserve.h               まとめ include
│  ├─ app.h                    App / Options
│  ├─ http.h                   Request / Response / Handler / Affinity
│  ├─ module.h                 IModule / ApiRegistry
│  ├─ browser.h                BrowserChannel（push / call）
│  ├─ repl.h                   ReplRegistry（コマンド追加）
│  ├─ json.h                   最小 JSON（生成中心 + 読取）
│  ├─ log.h
│  └─ plugin_abi.h             動的プラグインの C ABI（構造体のみ）
├─ src/
│  ├─ core/       app.cpp options.cpp log.cpp taskqueue.cpp session.cpp
│  ├─ http/       server.cpp socket.cpp parse.cpp response.cpp mime.cpp url.cpp sse.cpp
│  ├─ webroot/    webroot.cpp dirsource.cpp zipsource.cpp embedded.cpp
│  ├─ browser/    channel.cpp launcher_win.cpp launcher_posix.cpp
│  ├─ repl/       repl.cpp commands.cpp stdin_channel.cpp file_channel.cpp
│  ├─ json/       json.cpp
│  └─ plugin/     registry.cpp loader.cpp
├─ modules/
│  └─ fs/         fs_module.cpp     標準: ローカルファイルアクセス API
├─ app/
│  └─ main.cpp                      ベースアプリ実行ファイル
├─ web/                             ベースアプリ UI（開発時はここを直接配信）
│  ├─ index.html  style.css  app.js
│  └─ lib/appserve.js               ★ 標準 JS ランタイム（派生アプリも必ず使う）
├─ docs/          DESIGN.md REPL.md
└─ tests/
```

依存は **標準ライブラリ + OS ソケットのみ**。vcpkg も FetchContent も不要で、
CMake と C++17 コンパイラだけでビルドできる。

> 当初は zip 読み出しに miniz を vendoring する想定だったが、必要なのは
> 「読む」側だけ（zip の生成はビルド時に `cmake -E tar` が行う）で、
> stored + inflate に絞れば 400 行弱で書ける。`src/webroot/zip.cpp` に
> 自前実装を置き、third_party を丸ごと無くした。inflate は zlib の puff と
> 同じ「符号長を 1 bit ずつ伸ばして比較する」方式で、テーブル引きより遅い
> 代わりに短く検証しやすい（起動時に UI アセットを 1 回展開するだけなので
> 速度は問題にならない）。CRC-32 で展開結果を検証している。

---

## 4. HTTP サーバ

### 4.1 krkrz から引き継ぐもの / 変えるもの

| 項目 | krkrz | appserve |
|---|---|---|
| accept モデル | thread-per-connection | 同じ（ローカルツールには十分） |
| ルーティング | prefix 最長一致 | 同じ。ハンドラ/静的マウントを同一表で比較 |
| 静的配信 | Storages 経由 | `WebRoot` 抽象（dir / zip / embedded） |
| MIME | 小テーブル | 同じ（+ wasm/woff2/map/mp4 等を追加） |
| パス安全化 | `PathIsUnsafe`（`..` `\` `:` 拒否） | そのまま流用 |
| SSE | `SseFrame` + リングバックログ | そのまま流用 |
| 接続 | `Connection: close` 固定 | **keep-alive 対応**（ポーリングで TIME_WAIT が枯れるため） |
| ハンドラ | TJS クロージャ | `std::function` + スレッドアフィニティ指定 |
| 認証 | 無し | **トークン + Origin 検証**（FS API を晒すため必須） |
| ブラウザ起動 | `msedge --app` → `chrome --app` → 既定 | 同じ。ポート自動割当と組み合わせ |

### 4.2 ハンドラ API

```cpp
namespace appserve {

enum class Affinity { Main, Any };   // Main = メインスレッドで実行（既定）

struct Request {
    std::string        method;       // "GET" / "POST" ...
    std::string        path;         // URL デコード済み、クエリ除去済み
    std::string        suffix;       // マッチした prefix を除いた残り
    std::string        query;        // 生クエリ文字列
    std::string        body;         // 生バイト列（バイナリ可）
    Headers            headers;      // 小文字キー
    std::string        param(const char* key) const;   // クエリ取得（URL デコード）
    const Json&        json() const;                    // body を JSON として遅延パース
};

struct Response {
    int          status = 200;
    std::string  mime   = "application/json; charset=utf-8";
    std::string  body;
    Headers      headers;

    static Response json(const Json& v);
    static Response text(std::string s);
    static Response bytes(std::string data, std::string mime);
    static Response error(int status, std::string message);   // {"error":"..."} 形式
    static Response noContent();
};

using Handler = std::function<Response(const Request&)>;

} // namespace appserve
```

登録:

```cpp
app.route("/api/fs/list", Affinity::Main, [](const Request& r) {
    return Response::json(listDir(r.param("path")));
});
```

ハンドラ内の C++ 例外は 500 に変換し、`error` レベルでログへ出す
（krkrz が TJS 例外に対してやっていた処理と同じ扱い）。

### 4.3 内蔵ルート

| パス | 説明 |
|---|---|
| `GET /` `GET /<any>` | WebRoot から静的配信（最後にマッチ。SPA 用に `--spa` で index.html フォールバック） |
| `GET /_app/info` | ポート・バージョン・アプリ名・機能フラグ |
| `GET /_app/hello` | セッション確立。`sid` 発行 |
| `GET /_app/poll?sid&since` | **ロングポーリング**。サーバ→ブラウザのコマンド取得（既定 15s タイムアウト） |
| `POST /_app/result` | ブラウザ→サーバ のコマンド実行結果返却 |
| `POST /_app/bye` | 明示的な離脱通知（`sendBeacon`）。即座にセッション削除 |
| `GET /_app/events` | ログ/イベントの SSE（バックログ付き）。ポーリングの代替 |
| `GET /_app/sub/<ch>` | 汎用 SSE チャネル購読（`BrowserChannel::broadcast` の配信先） |
| `POST /_app/repl` | REPL コマンドを HTTP 経由で実行（`curl` でエージェント駆動できる） |

`/_app/*` 以下は将来の内蔵機能追加のために予約。派生アプリは `/api/...` を使う。

### 4.4 セキュリティ

localhost bind でも、同一マシン上の任意のプロセスや、ブラウザで開いた悪意ある
Web ページ（CSRF / DNS rebinding）からローカルファイル API を叩けてしまう。

1. 起動時に 128bit のランダムトークンを生成
2. ブラウザは `http://127.0.0.1:PORT/?t=<token>` で開かれる
3. `appserve.js` がトークンを `sessionStorage` に退避 → URL からは除去 →
   以降の全リクエストに `X-App-Token` ヘッダを付与
4. サーバは `/_app/*` と `/api/*` でトークンを検証。静的配信は素通し
5. `Origin` ヘッダがある場合は自サーバ由来かを検証（他サイトからの fetch を拒否）
6. `--no-token` で無効化可（CI やデバッグ用）
7. 既定 bind は `127.0.0.1`。それ以外を指定したら起動ログに警告
   （krkrz の非 loopback バインド警告と同じ方針）

---

## 5. UI の供給（開発時 / リリース時）

`WebRoot` は複数のソースを優先順位付きで解決する:

| 優先 | ソース | 用途 |
|---|---|---|
| 1 | `--web-root=DIR` | 明示指定 |
| 2 | カレントディレクトリの `web/` | **開発中**（指示書の要件） |
| 3 | 実行ファイルと同階層の `web/` | インストール後の差し替え |
| 4 | 実行ファイルと同階層の `<exe名>.zip` / `web.zip` | UI だけ後から更新 |
| 5 | exe に埋め込まれた zip | **リリース時の単一 exe**（指示書の要件） |

最初に見つかったものを使い、`.web` REPL コマンドで解決結果を確認できる。
開発中は 2 が勝つので `web/` を編集してブラウザをリロードするだけで反映される
（キャッシュ無効化のため dir ソースは `Cache-Control: no-cache` を付ける）。

### 埋め込みの仕組み

```cmake
appserve_embed_web(myapp WEB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/web)
```

ビルド時に `web/` を `cmake -E tar cf ... --format=zip` で zip 化し、
`bin2c` 相当の CMake スクリプトで `const unsigned char kEmbeddedWeb[]` を含む
`.cpp` を生成してターゲットにリンクする。

- exe 末尾追記方式ではなくリンク方式にする理由: プラットフォーム非依存で、
  コード署名を壊さず、ビルドシステムだけで完結する
- 「zip 化して実行ファイルとくっつけて提供」という要件は単一 exe になれば満たされる
- 差し替えたい場合は優先度 4（隣に `web.zip` を置く）で上書きできる

zip の読み出しは miniz。展開はメモリ上で行い、ファイルには一切書き出さない。

---

## 6. ブラウザ起動とライフサイクル

### 6.1 ポート

既定は `--port=0` = **OS に空きポートを割り当てさせる**（bind 後 `getsockname`
で実ポートを取得）。複数のツールを同時に立ち上げても衝突しない。
`--port=8899` のように明示指定も可。`--host` と併せて krkrz と同じ
`host:port` / `port` 書式を受ける。

### 6.2 起動シーケンス

```
listen(127.0.0.1:0) → 実ポート確定 → トークン生成 → ルート登録完了
  → ブラウザ起動（--browser=app が既定）
      1. msedge.exe  --app=http://127.0.0.1:PORT/?t=TOKEN
      2. chrome.exe  --app=...            （1 が起動できなければ）
      3. OS 既定ブラウザで URL を開く      （2 も駄目なら）
  → メインループ
```

Windows は `ShellExecute`（App Paths 解決込み）、mac は `open -a`、Linux は
`xdg-open` / `google-chrome --app`。krkrz の `TVPExecuteProgram` /
`TVPShellExecute` の 2 系統分離をそのまま踏襲する。

`--app` モードは独立ウィンドウ・アドレスバー無しで、ローカルアプリらしい見た目になる。
ユーザプロファイルを汚さないよう `--user-data-dir` は既定では**付けない**
（付けると毎回新規プロファイルになり起動が遅い）。`--browser-arg=` で追加可。

### 6.3 自動終了

```
SessionMgr:
  セッション = ブラウザ 1 タブ。/_app/hello で発行、poll / SSE / hb で last_seen 更新
  last_seen が session_ttl(既定 20s) を超えたら死亡とみなす
  /_app/bye 受信で即座に削除

自動終了条件（毎秒 main ループで判定）:
  起動から startup_grace(既定 30s) 経過している
  かつ 生存セッション数 == 0 の状態が idle_timeout(既定 10s) 継続
  → 終了
```

- リロードやページ遷移で一瞬 0 になるので `idle_timeout` の猶予を持たせる
- ブラウザ起動に失敗して誰も繋がなかった場合も `startup_grace` 後に終了する
  （プロセスが残らない）
- `--idle-timeout=0` で無効化（開発中・REPL 常駐時に使う）
- REPL がアタッチされている間は自動終了を抑止（`--repl` / `--replfile` 時）

---

## 7. 拡張機構（プラグイン構造）

2 層で提供する。**派生アプリは 7.1、後付け配布は 7.2**。

### 7.1 静的モジュール（推奨）

```cpp
// psdapp/psd_module.cpp
#include <appserve/appserve.h>

class PsdModule : public appserve::IModule {
public:
    const char* name() const override { return "psd"; }

    void registerApi(appserve::ApiRegistry& reg) override {
        reg.route("/api/psd/open",   Affinity::Main, [this](auto& r){ return open(r); });
        reg.route("/api/psd/tree",   Affinity::Main, [this](auto& r){ return tree(r); });
        reg.route("/api/psd/layer/", Affinity::Main, [this](auto& r){ return layer(r); });
        reg.replCommand("psd", "PSD 状態を表示", [this](auto args){ return status(); });
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
    if (!app.parseArgs(argc, argv)) return 1;
    app.options().appName = "PSD Inspector";
    app.addModule(std::make_unique<appserve::FsModule>());   // 標準 FS API を再利用
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

派生アプリの UI は自前の `web/` を持ち、`lib/appserve.js` だけ本体からコピー
（または `appserve_embed_web(... WITH_RUNTIME)` で自動合成）する。

### 7.2 動的プラグイン（DLL / so）

起動時に `--plugin-dir`（既定: 実行ファイル隣の `plugins/`）から
`*.dll` / `*.so` / `*.dylib` を読み、C ABI エントリを呼ぶ。
ABI 安定性のため **やり取りは POD 構造体と関数ポインタのみ**（C++ 型を跨がせない）。

```c
/* include/appserve/plugin_abi.h */
#define APPSERVE_ABI_VERSION 1

typedef struct AppserveStr { const char* ptr; size_t len; } AppserveStr;

typedef struct AppserveReq {
    AppserveStr method, path, suffix, query, body;
    AppserveStr (*header)(void* self, const char* key);
    void*       self;
} AppserveReq;

typedef struct AppserveResp {
    int          status;
    AppserveStr  mime;
    AppserveStr  body;          /* プラグインが確保、free_body で解放 */
    void       (*free_body)(void* p);
} AppserveResp;

typedef struct AppserveHost {
    int    abi_version;
    void*  ctx;
    void (*route)(void* ctx, const char* prefix, int affinity,
                  void (*fn)(const AppserveReq*, AppserveResp*, void* user), void* user);
    void (*broadcast)(void* ctx, const char* channel, const char* payload);
    void (*log)(void* ctx, int level, const char* msg);
    void (*repl_command)(void* ctx, const char* name, const char* help,
                         void (*fn)(const char* args, AppserveResp*, void* user), void* user);
} AppserveHost;

/* プラグインが export する唯一のシンボル */
APPSERVE_EXPORT int appserve_plugin_init(const AppserveHost* host);
```

`abi_version` 不一致のプラグインはロードせず警告ログを出す。

---

## 8. REPL / エージェント駆動

指示書の要点は 2 つ:
1. サーバ起動側から状態確認できる REPL
2. ブラウザ側にポーリング機構を標準搭載し、**REPL からブラウザを制御できる**

TJS のようなスクリプトエンジンは持たないので、サーバ側 REPL は
**ドットコマンド式**（krkrz の `.cap` `.dlg` `.click` に相当）とし、
任意コード評価が必要な場面は「ブラウザ側で JS を評価する」経路に寄せる。
UI ロジックが全て JS 側にある構成なので、これで実用上ほぼ困らない。

### 8.1 チャネル（3 系統。同時起動可）

| チャネル | 起動 | 用途 |
|---|---|---|
| stdin | `--repl` | 人間が対話 |
| ファイル | `--replfile=DIR` | **AI エージェント / CI**。krkrz と同一プロトコル |
| HTTP | 常時（`POST /_app/repl`） | `curl` から。ブラウザ内のデバッグコンソールからも |

ファイルチャネルは krkrz `-replfile` のプロトコルをそのまま使う（実績があり、
エージェント側の手順を共有できる）:

1. エージェント: コマンドを `cmd.tmp` に書き `cmd` に rename
2. サーバ: 検出→読取→削除→実行→結果 JSON を `resp.tmp` に書き `resp` に rename
3. エージェント: `resp` の出現を待ち、読取→削除

結果 JSON: `{"ok":bool,"result":"...","error":"..."}`。未読の `resp` が残る間は
次コマンドを処理しない（取りこぼし防止）。

### 8.2 サーバ側コマンド

| コマンド | 説明 |
|---|---|
| `.help` | 一覧（モジュールが追加したコマンドも含む） |
| `.info` | ポート / URL / トークン / uptime / バージョン |
| `.routes` | 登録ルート一覧（prefix, affinity, 登録元モジュール） |
| `.web` | WebRoot の解決結果（dir / zip / embedded とパス） |
| `.sessions` | 接続中ブラウザ（sid / UA / last_seen / 保留コマンド数） |
| `.stat` | リクエスト数 / エラー数 / 接続スレッド数 / メモリ |
| `.get <path>` | 自サーバへ内部 GET（HTTP を経由せず router を直接叩く） |
| `.post <path> [body]` | 同上 POST |
| `.push <ch> <text>` | `/_app/sub/<ch>` の購読者へ SSE 配信 |
| `.log [level]` | ログレベル表示 / 変更 |
| `.open [url]` | ブラウザを開き直す |
| `.quit` | 終了 |

### 8.3 ブラウザ制御コマンド（`.b` プレフィックス）

サーバ → ブラウザの往復は「ポーリングで取りに来させ、結果を POST で返させる」:

```
REPL:    .b eval document.title
  ↓ BrowserChannel::call("eval", {code:"document.title"}, 5s) がブロック
  ↓ 保留中の /_app/poll が即座に {"id":7,"cmd":"eval","arg":{...}} を返す
  ↓ appserve.js が実行し POST /_app/result {"id":7,"ok":true,"value":"PSD Inspector"}
  ↑ call() が解けて REPL が結果を表示
REPL:    "PSD Inspector"
```

`appserve.js` が標準で処理するコマンド:

| コマンド | REPL 書式 | 説明 |
|---|---|---|
| `eval` | `.b eval <js>` | JS 式/文を評価して結果を JSON 化して返す |
| `dom` | `.b dom <selector>` | `outerHTML` を返す（長さ上限あり） |
| `text` | `.b text <selector>` | `innerText` を返す |
| `click` | `.b click <selector>` | 実イベントを dispatch |
| `set` | `.b set <selector> <value>` | input/select に値を入れ `input`/`change` を発火 |
| `state` | `.b state` | アプリが `app.exposeState(fn)` で公開した状態を返す |
| `call` | `.b call <name> [json]` | `app.command(name, fn)` で登録された関数を呼ぶ |
| `nav` | `.b nav <path>` | 画面遷移 |
| `err` | `.b err` | 直近のフロント例外ログ |

これで **AI が UI の実状態を観測しながらフロント/バックエンド両方を直せる**。
スクリーンショットはブラウザの制約で取れないため、`dom` / `state` を観測手段とする
（krkrz の `Agent.dialogTree` に相当する立ち位置）。

### 8.4 `appserve.js` 標準ランタイム

```js
import { app } from './lib/appserve.js';

await app.ready();                       // hello → sid 取得 → poll 開始
const r = await app.get('/api/fs/list', { path: 'D:/' });
await app.post('/api/psd/open', { path: file });

app.command('reload', () => refreshTree());     // .b call reload から呼べる
app.exposeState(() => ({ cwd, selected, tree: tree.length }));   // .b state
app.on('progress', d => bar.value = d.pct);     // /_app/sub/progress の SSE
```

- トークン付与、エラーの統一処理、poll の再接続（指数バックオフ）、
  `beforeunload` での `sendBeacon('/_app/bye')` を全て内包する
- 派生アプリはこのファイルをそのまま使う。UI フレームワークは不問（素の JS で書ける）
- poll と SSE の両方を張る（poll = サーバ→ブラウザのコマンド、SSE = イベント配信）。
  SSE が使えない環境では poll だけで縮退動作する

---

## 9. 標準モジュール: ローカルファイルアクセス

### API

| ルート | 説明 |
|---|---|
| `GET /api/fs/roots` | ドライブ一覧 / ホーム / よく使う場所 |
| `GET /api/fs/list?path=&glob=` | 列挙 → `[{name,size,mtime,dir,link}]` |
| `GET /api/fs/stat?path=` | 単体情報 |
| `GET /api/fs/read?path=&offset=&length=` | バイナリ読み出し（Range 相当） |
| `GET /api/fs/text?path=&enc=` | テキストとして読む（UTF-8 / CP932 判定つき） |
| `GET /api/fs/download?path=` | `Content-Disposition: attachment` |
| `POST /api/fs/write` | 書き込み（既定 OFF、`--allow-write` で有効） |
| `POST /api/fs/mkdir` `/api/fs/remove` | 同上 |
| `GET /api/fs/watch?path=` | 変更通知（`/_app/sub/fs` へ push）。第 2 段階 |

- パスは全て UTF-8 文字列。Windows では `std::filesystem::u8path` 経由で扱う
  （日本語パス必須）
- `--root=DIR`（複数可）でアクセス可能なルートを制限できる。未指定なら無制限
  （ローカルツール前提。ただし 4.4 のトークン検証は常に効く）
- 大きいファイルの全読みを避けるため `read` は上限つき（既定 64MB、超過は 413）

### UI

- 左: ディレクトリツリー（遅延展開）
- 中: ファイル一覧（ソート / フィルタ）
- 右: プレビュー（テキスト / hex / 画像 / 情報）
- パンくず + パス直接入力 + ドライブ切替

これがそのまま「派生アプリのファイル選択部品」として再利用される
（psdapp は `/api/fs/*` で PSD を選び、`/api/psd/open` に渡す）。

---

## 10. コマンドラインオプション

```
appserve [options]

  --host=ADDR           bind アドレス（既定 127.0.0.1。"0.0.0.0" で全 IF ＋警告）
  --port=N              ポート（既定 0 = OS が空きを自動割当）
  --listen=HOST:PORT    上記 2 つの短縮形（krkrz -replweb と同じ書式）

  --web-root=DIR        UI ディレクトリを明示指定
  --spa                 未知パスを index.html にフォールバック

  --browser=app|default|none   起動方式（既定 app）
  --browser-arg=ARG            ブラウザへ追加引数（複数可）
  --no-browser                 = --browser=none

  --idle-timeout=SEC    接続 0 が続いたら終了（既定 10、0 で無効）
  --startup-grace=SEC   起動直後の猶予（既定 30）
  --session-ttl=SEC     セッション死亡判定（既定 20）

  --repl                stdin REPL を有効化
  --replfile=DIR        ファイルチャネル REPL
  --log-level=verbose|debug|info|warn|error
  --log-file=PATH

  --token=STR / --no-token
  --root=DIR            FS API の許可ルート（複数可）
  --allow-write         FS 書き込み API を有効化
  --plugin-dir=DIR      動的プラグイン探索先

  --version  --help
```

派生アプリは `App::addOption()` で独自オプションを足せる。

---

## 11. 実装フェーズ

| # | 内容 | 完了条件 | 状態 |
|---|---|---|---|
| 1 | 骨格 + HTTP サーバ | CMake 通る。`--port=0` で listen し `/_app/info` が JSON を返す | ✅ |
| 2 | WebRoot（dir）+ ブラウザ起動 + アイドル終了 | `web/index.html` が `--app` ウィンドウで開き、閉じると数秒でプロセスが消える | ✅ |
| 3 | JSON + Router + Affinity + トークン | main スレッド実行のハンドラが書ける。他オリジンからは 403 | ✅ |
| 4 | `appserve.js` + poll/SSE + BrowserChannel | ブラウザから API 呼び出し、サーバから push が届く | ✅ |
| 5 | REPL（stdin / file / http）+ `.b` コマンド | `.b eval` `.b dom` が往復する。エージェント駆動が成立 | ✅ |
| 6 | FS モジュール + ファイルブラウザ UI | **指示書のベースアプリ完成** | ✅ |
| 7 | zip WebRoot + `appserve_embed_web()` | `web/` 無しで単一 exe が動く | ✅ |
| 8 | 動的プラグイン ABI | `plugins/*.dll` が API を足せる | ⬜ 実装済 / 実プラグイン未検証 |
| 9 | psdapp の実証 | psdparse を API 化した派生アプリが動く | ⬜ |

### 実機で確認した動作（Windows / MSVC 19.44）

| 項目 | 結果 |
|---|---|
| 静的配信 + トークン検証 | `/` は素通し、`/_app/info` はトークン無しで 401 |
| Origin 検証 | `Origin: http://evil.example` で 403 |
| FS API | list / text / roots / write 拒否。日本語パス・UTF-8 判定 OK |
| ファイルチャネル REPL | `.info` `.routes` `.sessions` `.stat` `.quit` |
| ブラウザ往復 | `.b eval` `.b state` `.b call` が実ブラウザと往復 |
| ブラウザ起動 | Edge が `--app` モードで開きセッション確立 |
| SSE | サーバログがブラウザのログパネルへ配信 |
| 埋め込み zip | `web/` の無い場所で exe 単体起動、展開結果が原本と md5 一致 |
| アイドル自動終了 | grace 3s + idle 3s で 3 秒後に自動終了 |

---

## 12. 決定事項

| 項目 | 決定 | 理由 |
|---|---|---|
| ライセンス | MIT | |
| C++ 標準 | 17 | psdparse・krkrz と揃える |
| JSON 実装 | 自前最小（`src/json/`） | 依存ゼロを維持。不足したら差し替え |
| zip 読み出し | 自前（`src/webroot/zip.cpp`） | 読む側だけで足りるので miniz を落とした（上記 §3 の注記） |
| UI フレームワーク | 素の JS | 本体は不問。派生アプリの自由 |
| 文字コード変換 | 自前 | UTF-8 ⇔ UTF-16 のみ。CP932 は「UTF-8 として妥当でなければ」の簡易判定 |
| ログ | 自前 | レベル + sink。SSE へ流す sink を標準装備 |
| 行編集ライブラリ | 使わない | 依存ゼロ方針。人間は端末の行編集、エージェントはファイル/HTTP チャネル |

### 既知の制限

- `Transfer-Encoding: chunked` のリクエストは 501（ローカル UI からは使われない）
- zip64 アーカイブは非対応（UI アセットで 4GB を超えることはない）
- ブラウザのスクリーンショットは取得不可。UI 観測は `.b dom` / `.b state`
- 接続 1 本 = 1 スレッド。ローカルツール前提の割り切り
