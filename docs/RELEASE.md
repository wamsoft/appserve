# 配布とリリース

appserve ベースのアプリは **UI を埋め込んだ実行ファイル 1 つ**で動くので、
配布物も「exe + README + LICENSE」だけで済む。ランタイム DLL も、
別途置くデータフォルダも要らない。

- 開発中: カレント/exe 隣の `web/` を読む → 編集してリロードすれば反映
- 配布時: `appserve_embed_web()` が `web/` を zip 化して exe に埋め込む

---

## 1. パッケージの設定 (CMake)

```cmake
add_executable(myapp src/main.cpp)
target_link_libraries(myapp PRIVATE appserve::core)

appserve_embed_web(myapp WEB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/web)

appserve_package(myapp
    VERSION     ${MYAPP_VERSION}
    DESCRIPTION "My tool")
appserve_package_finalize()      # include(CPack)。最後に 1 回だけ
```

`appserve_package()` が行うこと:

| | |
|---|---|
| `install()` | exe をフラットに + README.md / CHANGELOG.md / LICENSE があれば同梱 |
| パッケージ名 | `<name>-<version>-<os>-<arch>` (windows/linux/macos × x64/x86/arm64) |
| ジェネレータ | Windows: `ZIP` (+ NSIS があれば `NSIS`) / Linux: `TGZ` `ZIP` / macOS: `ZIP` `TGZ` |

オプション: `NAME` `VENDOR` `LICENSE` `EXTRA_FILES`、インストーラ不要なら `NO_INSTALLER`。

### 手元でパッケージを作る

```bash
cmake --preset windows
cmake --build --preset windows-rel
cpack --config build/windows/CPackConfig.cmake -C Release -B dist
```

`-B dist` を付けないとカレントに成果物が落ちるので注意。

NSIS (`makensis`) が見つからない環境では zip だけが作られ、configure 時に
`makensis not found — packaging ZIP only` とログが出る。インストーラも手元で
試したい場合は [NSIS](https://nsis.sourceforge.io/) を入れる
(`choco install nsis`)。

### 中身の確認

```
myapp-0.1.0-windows-x64/
  myapp.exe          UI 埋め込み済み。これ単体で動く
  README.md
  LICENSE
```

zip を展開して `myapp.exe` を叩き、ブラウザが開けば配布物として成立している。
`--web-root` を指定しなければ埋め込み zip が使われる (`.web` REPL コマンドで
`embedded` と出ることを確認できる)。

---

## 2. GitHub でのリリース

`.github/workflows/release.yml` がタグを起点に全部やる。

```bash
git tag v0.1.0
git push origin v0.1.0
```

これで走る処理:

1. `windows-latest` と `ubuntu-latest` で並列にビルド
2. Windows は NSIS を用意 (ランナーに無ければ `choco install nsis`)
3. タグから `v` を落としたバージョンを `-DAPPSERVE_VERSION=` で渡す
4. `cpack` で zip / インストーラ / tar.gz を生成
5. 成果物をアーティファクトへアップロード
6. `publish` ジョブが全部を集めて `gh release create` でリリースを作り、
   コミットログから自動生成したノートを付ける

やり直したいときは、同じタグで再実行すれば `gh release upload --clobber` で
差し替わる。タグを打たずに動作だけ試すなら **Actions → release →
Run workflow** (`workflow_dispatch`) を使う。この場合パッケージは作られるが
リリースは作られない (`publish` は `push` イベントのときだけ動く)。

### Linux ビルドの扱い

Linux は現状 **未検証**なので `continue-on-error: true` にしてある。
落ちても Windows のリリースは公開される。緑になったのを確認したら
`experimental: false` に変えて必須にすること。

### バージョンの整合

`appserve_package(VERSION ...)` に渡す値は CMake のキャッシュ変数から来る:

- appserve: `APPSERVE_VERSION` (既定 `0.1.0`)
- 派生アプリ: 同様に `<APP>_VERSION` を作って `-D` で上書きできるようにする

タグと CMakeLists の既定値がずれていても、リリースはタグの値が正になる。
節目では既定値もタグに合わせて上げておくと、手元ビルドの表示と揃う。

---

## 3. 派生アプリで依存を固定する

派生アプリが appserve を FetchContent で取っている場合、既定は `master` を
追いかける。**リリースの再現性を確保したいときはタグに固定する**:

```bash
cmake -B build -DPSDTEXT_APPSERVE_TAG=v0.1.0 -DPSDTEXT_PSDPARSE_TAG=v0.8.1
```

あるいは `cmake/Dependencies.cmake` の `*_TAG` の既定値を書き換える。
開発中は `../appserve` のチェックアウトが自動で優先されるので、この設定は
CI とリリースにだけ効く。

---

## 4. コード署名 (未対応)

Windows では未署名 exe が SmartScreen に止められる。必要になったら
`release.yml` のパッケージ手順の前に `signtool` を挟む:

```yaml
- name: Sign
  run: signtool sign /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 `
       /f cert.pfx /p ${{ secrets.CERT_PASSWORD }} build/Release/myapp.exe
```

UI を exe にリンクで埋め込んでいる (末尾追記ではない) のは、この署名を
壊さないためでもある。
