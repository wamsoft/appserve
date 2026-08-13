//---------------------------------------------------------------------------
// ブラウザ起動
//
// 吉里吉里Z の TVPExecuteProgram / TVPShellExecute の 2 系統分離を踏襲する:
//   - 「プログラムを引数付きで実行する」 → Edge / Chrome の --app モード
//   - 「URL を既定ハンドラで開く」       → 通常のブラウザウィンドウ
// アプリモードが使えない環境では自動的に後者へフォールバックする。
//---------------------------------------------------------------------------
#pragma once
#include <string>
#include <vector>

namespace appserve {
namespace browser {

/// Edge → Chrome の順に --app=<url> で起動を試みる。起動できたら true。
bool launchAppMode(const std::string& url, const std::vector<std::string>& extraArgs);

/// OS 既定のブラウザで URL を開く。
bool openDefault(const std::string& url);

/// appMode=true なら launchAppMode → 失敗時 openDefault。
bool open(const std::string& url, bool appMode,
          const std::vector<std::string>& extraArgs = {});

} // namespace browser
} // namespace appserve
