//---------------------------------------------------------------------------
// 最小 zip リーダ (読み出し専用)
//
// 埋め込み UI アーカイブを展開するためだけに使う。zip の生成はビルド時に
// `cmake -E tar cf ... --format=zip` が行うので、ここでは
//   - stored (method 0)
//   - deflate (method 8)
// の展開だけを実装する。外部ライブラリ (miniz / zlib) に依存しないことで
// 「標準ライブラリ + OS ソケットのみ」という依存方針を保つ。
//---------------------------------------------------------------------------
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace appserve {
namespace zip {

/// 生 DEFLATE ストリーム (zlib ヘッダ無し) を展開する。
/// expectedSize が非 0 ならその大きさで出力を予約し、超過をエラーにする。
bool inflateRaw(const uint8_t* src, size_t srcLen, std::string& out, size_t expectedSize);

/// CRC-32 (IEEE)。展開結果の検証に使う。
uint32_t crc32(const uint8_t* data, size_t len);

//---------------------------------------------------------------------------
struct Entry {
	std::string name;            ///< アーカイブ内パス ("index.html", "lib/appserve.js")
	uint16_t    method = 0;      ///< 0 = stored / 8 = deflate
	uint32_t    compressedSize = 0;
	uint32_t    uncompressedSize = 0;
	uint32_t    crc = 0;
	uint32_t    localOffset = 0;
};

//---------------------------------------------------------------------------
/// メモリ上の zip を読む。データの寿命は呼び出し側が保証すること
/// (埋め込みアーカイブは静的データなので問題ない)。
class Reader {
public:
	/// 中央ディレクトリを読み込む。失敗理由は error() に入る。
	bool open(const uint8_t* data, size_t size);
	bool isOpen() const { return data_ != nullptr; }
	const std::string& error() const { return error_; }

	const std::vector<Entry>& entries() const { return entries_; }
	const Entry* find(const std::string& name) const;

	/// 展開して out へ。CRC が合わなければ false。
	bool extract(const Entry& e, std::string& out) const;
	bool extract(const std::string& name, std::string& out) const;

private:
	const uint8_t*     data_ = nullptr;
	size_t             size_ = 0;
	std::vector<Entry> entries_;
	mutable std::string error_;
};

} // namespace zip
} // namespace appserve
