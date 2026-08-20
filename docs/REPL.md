# REPL / エージェント駆動

appserve は「サーバの状態を確認する」だけでなく、**REPL からブラウザ側を
観測・操作する**ところまでを標準機能として持つ。UI ロジックが全て JS 側に
ある構成なので、これがあると AI エージェントはフロントとバックエンドの
両方を同じ 1 本のチャネルから直せる。

TJS のようなスクリプトエンジンは内蔵しないので、サーバ側はドットコマンド式。
任意コードの評価が必要な場面は `.b eval` (ブラウザ側で JS を評価) に寄せている。

---

## チャネル (3 系統。同時に使える)

| チャネル | 起動 | 用途 |
|---|---|---|
| stdin | `--repl` | 人間の対話 |
| ファイル | `--replfile=DIR` | **AI エージェント / CI** |
| HTTP | 常時 (`POST /_app/repl`) | `curl` / ブラウザ内デバッグ |

### ファイルチャネル (エージェント向け)

吉里吉里Z の `-replfile` と同一プロトコル。`<DIR>` 配下で lockstep に動く:

1. エージェント: コマンド (UTF-8) を `cmd.tmp` に書き、`cmd` に **rename**
2. サーバ: `cmd` を検出 → 読取 → 削除 → 実行 → 結果 JSON を `resp.tmp` に
   書き `resp` に **rename**
3. エージェント: `resp` の出現を待ち、読取 → **削除**。次のコマンドへ

結果 JSON: `{"ok":bool,"result":"...","error":"..."}`

> **未読の `resp` が残っている間、サーバは次の `cmd` を処理しない。**
> 取りこぼし防止のための仕様なので、エージェントは必ず `resp` を消してから
> 次を送ること。消し忘れると「古い応答を読み続ける」形で 1 つずれる。

bash からの最小ドライバ:

```bash
CHAN=/tmp/appserve-chan
repl() {
  printf '%s' "$1" > "$CHAN/cmd.tmp" && mv "$CHAN/cmd.tmp" "$CHAN/cmd"
  while [ ! -f "$CHAN/resp" ]; do sleep 0.05; done
  local out; out=$(cat "$CHAN/resp"); rm -f "$CHAN/resp"
  printf '%s\n' "$out"        # ← パイプで切ると rm が飛ぶので必ず変数経由で
}

appserve --replfile=$CHAN --no-browser --idle-timeout=0 &
repl '.info'
repl '.b eval document.title'
repl '.quit'
```

### HTTP チャネル

```bash
curl -s -X POST -H "X-App-Token: $TOKEN" -d '.stat' http://127.0.0.1:PORT/_app/repl
# JSON ボディ {"cmd":"..."} も受ける
```

---

## サーバ側コマンド

| コマンド | 説明 |
|---|---|
| `.help` | 一覧 (モジュール/プラグインが足したものも含む) |
| `.info` | URL / トークン / uptime / web root / 各種設定 |
| `.routes` | 登録ルートと静的マウント (affinity と登録元つき) |
| `.web [list]` | UI アセットの解決結果。`list` で収録ファイル一覧 |
| `.sessions` | 接続中ブラウザ (sid / 最終確認 / 保留コマンド数 / UA) |
| `.stat` | リクエスト数 / エラー数 / 接続数 / SSE 数 / 保留タスク数 |
| `.get <path>` | ルータを直接叩く (HTTP を経由しない)。JSON は整形表示 |
| `.post <path> [body]` | 同上 POST |
| `.push <ch> <text>` | `/_app/sub/<ch>` の購読者へ SSE 配信 |
| `.log [level]` | ログレベルの表示 / 変更 |
| `.open [url]` | ブラウザを開き直す |
| `.ls [path]` | ファイル API 経由でディレクトリ列挙 (fs モジュール) |
| `.quit` | サーバを停止 |

先頭の `.` は省略できる (`info` でも `.info` でも同じ)。

---

## ブラウザ制御コマンド (`.b`)

サーバ → ブラウザの往復は「ブラウザにポーリングで取りに来させ、結果を
POST で返させる」形で実現している。`appserve.js` の poll は保留中の
リクエストとして待機しているので、`.b` の応答は待ち時間なしで返る。

```
.b eval document.title
  ↓ BrowserChannel::call("eval", {code}) がブロック
  ↓ 保留中の /_app/poll が即座に {"id":7,"cmd":"eval",...} を返す
  ↓ appserve.js が実行し POST /_app/result で返す
  ↑ call() が解けて REPL が結果を表示
```

| コマンド | 説明 |
|---|---|
| `.b eval <js>` | JS を評価して結果を返す (式・文どちらも可) |
| `.b dom [selector]` | `outerHTML` (既定 `body`、長さ上限あり) |
| `.b text [selector]` | `innerText` |
| `.b click <selector>` | 実イベントを dispatch |
| `.b set <sel> <value>` | input に値を入れ `input`/`change` を発火 |
| `.b state` | `app.exposeState()` が返す状態 |
| `.b call <name> [json]` | `app.command(name, fn)` で登録された関数を呼ぶ |
| `.b nav <path>` | 画面遷移 |
| `.b err` | 直近のフロント例外 |
| `.b <other> [json]` | `app.handler(name, fn)` へ転送 (アプリ拡張用) |
| `.bhelp` | 上記のヘルプ |

`shutdown` も同じ経路のコマンドだが、これはサーバが終了直前に自分で投げる
(ブラウザに窓を閉じさせる)。手で打つ必要はない。

**スクリーンショットは取れない** (ブラウザの制約)。UI の状態確認は
`.b dom` / `.b text` / `.b state` で行う — 吉里吉里Z の `Agent.dialogTree`
に相当する立ち位置。

### 典型的なデバッグの流れ

```bash
repl '.b state'                     # UI が今どういう状態か
repl '.b text #listStatus'          # 画面に出ているメッセージ
repl '.b eval [...document.querySelectorAll(".row")].length'
repl '.b click .row:nth-child(3)'   # 操作して
repl '.b state'                     # 反映を確認
repl '.b err'                       # 例外が出ていないか
repl '.get /api/fs/list?path=D:/'   # サーバ側 API を直接確認
```

---

## アイドル自動終了と REPL

REPL (stdin / ファイルチャネル) がアタッチされている間は、ブラウザが 0 本に
なってもサーバは終了しない。開発中にブラウザを閉じてもセッションが続くので、
`--idle-timeout=0` を毎回書く必要はない。

HTTP チャネルだけの場合はアタッチとみなさないので、通常どおり自動終了する。

逆に**サーバが終了するとブラウザの窓も閉じる**ので、エージェント駆動で
`--browser` を有効にしたまま回しても死んだ窓が溜まらない。`.quit` なら即座、
強制終了なら数秒後 (通信断の検出) に閉じる。
