//---------------------------------------------------------------------------
// 最小 zip リーダ 実装
//
// inflate は canonical Huffman を「符号長を 1 bit ずつ伸ばしながら比較する」
// 方式 (zlib の puff と同じ) で書いている。テーブル引きより遅いが、コードが
// 短く検証しやすい。UI アセット数百 KB を起動時に 1 回展開するだけなので
// 速度は問題にならない。
//---------------------------------------------------------------------------
#include "webroot/zip.h"

#include <cstring>

namespace appserve {
namespace zip {

namespace {

//---------------------------------------------------------------------------
// ビットリーダ (LSB first)
struct BitReader {
	const uint8_t* src;
	size_t         len;
	size_t         pos = 0;      // 次に読むバイト
	int            bitcnt = 0;   // bitbuf に溜まっているビット数
	uint32_t       bitbuf = 0;
	bool           overrun = false;

	int bits(int need)
	{
		uint32_t val = bitbuf;
		while (bitcnt < need) {
			if (pos >= len) { overrun = true; return -1; }
			val |= (uint32_t)src[pos++] << bitcnt;
			bitcnt += 8;
		}
		bitbuf = val >> need;
		bitcnt -= need;
		return (int)(val & ((1u << need) - 1));
	}
};

//---------------------------------------------------------------------------
// canonical Huffman テーブル
struct Huffman {
	short count[16];
	short symbol[288];
};

int construct(Huffman& h, const short* length, int n)
{
	for (int len = 0; len <= 15; ++len) h.count[len] = 0;
	for (int sym = 0; sym < n; ++sym) h.count[length[sym]]++;
	if (h.count[0] == n) return 0;   // 全て未使用

	// 符号長の整合性チェック (over-subscribed なら負)
	int left = 1;
	for (int len = 1; len <= 15; ++len) {
		left <<= 1;
		left -= h.count[len];
		if (left < 0) return left;
	}

	short offs[16];
	offs[1] = 0;
	for (int len = 1; len < 15; ++len) offs[len + 1] = (short)(offs[len] + h.count[len]);
	for (int sym = 0; sym < n; ++sym)
		if (length[sym]) h.symbol[offs[length[sym]]++] = (short)sym;

	return left;   // 0 なら完全、正なら incomplete
}

int decodeSym(BitReader& br, const Huffman& h)
{
	int code = 0, first = 0, index = 0;
	for (int len = 1; len <= 15; ++len) {
		int b = br.bits(1);
		if (b < 0) return -1;
		code |= b;
		int count = h.count[len];
		if (code - count < first) return h.symbol[index + (code - first)];
		index += count;
		first += count;
		first <<= 1;
		code  <<= 1;
	}
	return -1;
}

//---------------------------------------------------------------------------
const short kLenBase[29] = {
	3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
	35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
const short kLenExtra[29] = {
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
	3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
const short kDistBase[30] = {
	1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
	257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
const short kDistExtra[30] = {
	0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
	7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

//---------------------------------------------------------------------------
bool inflateBlockCodes(BitReader& br, std::string& out,
                       const Huffman& lencode, const Huffman& distcode,
                       size_t maxOut)
{
	while (true) {
		int sym = decodeSym(br, lencode);
		if (sym < 0) return false;
		if (sym < 256) {
			if (maxOut && out.size() >= maxOut) return false;
			out += (char)(unsigned char)sym;
			continue;
		}
		if (sym == 256) return true;    // ブロック終端

		sym -= 257;
		if (sym >= 29) return false;
		int extra = br.bits(kLenExtra[sym]);
		if (extra < 0) return false;
		size_t length = (size_t)kLenBase[sym] + (size_t)extra;

		int dsym = decodeSym(br, distcode);
		if (dsym < 0 || dsym >= 30) return false;
		int dextra = br.bits(kDistExtra[dsym]);
		if (dextra < 0) return false;
		size_t dist = (size_t)kDistBase[dsym] + (size_t)dextra;
		if (dist > out.size()) return false;
		if (maxOut && out.size() + length > maxOut) return false;

		// 重なりコピー (LZ77) なので 1 バイトずつ
		size_t from = out.size() - dist;
		for (size_t i = 0; i < length; ++i) out += out[from + i];
	}
}

bool inflateFixed(BitReader& br, std::string& out, size_t maxOut)
{
	static Huffman lencode, distcode;
	static bool built = false;
	if (!built) {
		short lengths[288];
		int sym = 0;
		for (; sym < 144; ++sym) lengths[sym] = 8;
		for (; sym < 256; ++sym) lengths[sym] = 9;
		for (; sym < 280; ++sym) lengths[sym] = 7;
		for (; sym < 288; ++sym) lengths[sym] = 8;
		construct(lencode, lengths, 288);
		for (sym = 0; sym < 30; ++sym) lengths[sym] = 5;
		construct(distcode, lengths, 30);
		built = true;
	}
	return inflateBlockCodes(br, out, lencode, distcode, maxOut);
}

bool inflateDynamic(BitReader& br, std::string& out, size_t maxOut)
{
	static const short kOrder[19] =
		{16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

	int nlen  = br.bits(5); if (nlen  < 0) return false; nlen  += 257;
	int ndist = br.bits(5); if (ndist < 0) return false; ndist += 1;
	int ncode = br.bits(4); if (ncode < 0) return false; ncode += 4;
	if (nlen > 286 || ndist > 30) return false;

	short lengths[320];
	std::memset(lengths, 0, sizeof(lengths));
	for (int i = 0; i < ncode; ++i) {
		int v = br.bits(3);
		if (v < 0) return false;
		lengths[kOrder[i]] = (short)v;
	}
	for (int i = ncode; i < 19; ++i) lengths[kOrder[i]] = 0;

	Huffman lencode;
	if (construct(lencode, lengths, 19) != 0) return false;   // 完全でなければ不正

	// 符号長列そのものを Huffman で読む
	int index = 0;
	while (index < nlen + ndist) {
		int sym = decodeSym(br, lencode);
		if (sym < 0) return false;
		if (sym < 16) {
			lengths[index++] = (short)sym;
		} else {
			int len = 0, repeat = 0;
			if (sym == 16) {
				if (index == 0) return false;
				len = lengths[index - 1];
				int v = br.bits(2); if (v < 0) return false;
				repeat = 3 + v;
			} else if (sym == 17) {
				int v = br.bits(3); if (v < 0) return false;
				repeat = 3 + v;
			} else {
				int v = br.bits(7); if (v < 0) return false;
				repeat = 11 + v;
			}
			if (index + repeat > nlen + ndist) return false;
			while (repeat--) lengths[index++] = (short)len;
		}
	}
	if (lengths[256] == 0) return false;   // 終端記号が無い

	Huffman litcode, distcode;
	int err = construct(litcode, lengths, nlen);
	if (err && (err < 0 || nlen != litcode.count[0] + litcode.count[1])) return false;
	err = construct(distcode, lengths + nlen, ndist);
	if (err && (err < 0 || ndist != distcode.count[0] + distcode.count[1])) return false;

	return inflateBlockCodes(br, out, litcode, distcode, maxOut);
}

//---------------------------------------------------------------------------
// リトルエンディアン読み出し
uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

} // anonymous

//---------------------------------------------------------------------------
bool inflateRaw(const uint8_t* src, size_t srcLen, std::string& out, size_t expectedSize)
{
	BitReader br;
	br.src = src;
	br.len = srcLen;
	out.clear();
	if (expectedSize) out.reserve(expectedSize);

	while (true) {
		int last = br.bits(1);
		if (last < 0) return false;
		int type = br.bits(2);
		if (type < 0) return false;

		if (type == 0) {
			// stored: バイト境界へ揃えて LEN/NLEN を読む
			br.bitbuf = 0;
			br.bitcnt = 0;
			if (br.pos + 4 > br.len) return false;
			uint16_t len  = rd16(br.src + br.pos);
			uint16_t nlen = rd16(br.src + br.pos + 2);
			br.pos += 4;
			if ((uint16_t)~len != nlen) return false;
			if (br.pos + len > br.len) return false;
			if (expectedSize && out.size() + len > expectedSize) return false;
			out.append((const char*)br.src + br.pos, len);
			br.pos += len;
		} else if (type == 1) {
			if (!inflateFixed(br, out, expectedSize)) return false;
		} else if (type == 2) {
			if (!inflateDynamic(br, out, expectedSize)) return false;
		} else {
			return false;   // 予約値
		}

		if (last) break;
	}
	return true;
}

//---------------------------------------------------------------------------
uint32_t crc32(const uint8_t* data, size_t len)
{
	static uint32_t table[256];
	static bool built = false;
	if (!built) {
		for (uint32_t i = 0; i < 256; ++i) {
			uint32_t c = i;
			for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
			table[i] = c;
		}
		built = true;
	}
	uint32_t c = 0xFFFFFFFFu;
	for (size_t i = 0; i < len; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
	return c ^ 0xFFFFFFFFu;
}

//---------------------------------------------------------------------------
// zip 中央ディレクトリ
//---------------------------------------------------------------------------
bool Reader::open(const uint8_t* data, size_t size)
{
	data_ = nullptr;
	size_ = 0;
	entries_.clear();
	error_.clear();

	if (!data || size < 22) { error_ = "archive too small"; return false; }

	// EOCD (0x06054b50) を末尾から探す。コメント最大 64KB ぶん遡る。
	size_t maxBack = size < (22 + 65535) ? size : (22 + 65535);
	size_t eocd = 0;
	bool found = false;
	for (size_t back = 22; back <= maxBack; ++back) {
		const uint8_t* p = data + size - back;
		if (rd32(p) == 0x06054b50u) { eocd = size - back; found = true; break; }
	}
	if (!found) { error_ = "end of central directory not found"; return false; }

	const uint8_t* e = data + eocd;
	uint16_t count      = rd16(e + 10);
	uint32_t cdSize     = rd32(e + 12);
	uint32_t cdOffset   = rd32(e + 16);
	if (cdOffset == 0xFFFFFFFFu || cdSize == 0xFFFFFFFFu) {
		error_ = "zip64 archives are not supported";
		return false;
	}
	if ((size_t)cdOffset + cdSize > size) { error_ = "central directory out of range"; return false; }

	size_t p = cdOffset;
	entries_.reserve(count);
	for (uint16_t i = 0; i < count; ++i) {
		if (p + 46 > size) { error_ = "truncated central directory"; return false; }
		const uint8_t* h = data + p;
		if (rd32(h) != 0x02014b50u) { error_ = "bad central directory signature"; return false; }
		Entry en;
		en.method           = rd16(h + 10);
		en.crc              = rd32(h + 16);
		en.compressedSize   = rd32(h + 20);
		en.uncompressedSize = rd32(h + 24);
		uint16_t nameLen    = rd16(h + 28);
		uint16_t extraLen   = rd16(h + 30);
		uint16_t commentLen = rd16(h + 32);
		en.localOffset      = rd32(h + 42);
		if (p + 46 + nameLen > size) { error_ = "truncated entry name"; return false; }
		en.name.assign((const char*)h + 46, nameLen);
		// zip の区切りは "/" 固定だが念のため正規化
		for (char& c : en.name) if (c == '\\') c = '/';
		if (!en.name.empty() && en.name.back() != '/')   // ディレクトリ項目は持たない
			entries_.push_back(std::move(en));
		p += 46u + nameLen + extraLen + commentLen;
	}

	data_ = data;
	size_ = size;
	return true;
}

const Entry* Reader::find(const std::string& name) const
{
	for (const auto& e : entries_) if (e.name == name) return &e;
	return nullptr;
}

bool Reader::extract(const Entry& e, std::string& out) const
{
	if (!data_) { error_ = "archive not open"; return false; }
	// ローカルヘッダを読んで実データ位置を確定する (extra 長が中央ディレクトリと
	// 異なることがあるため、ローカル側の値を使うのが正しい)
	if ((size_t)e.localOffset + 30 > size_) { error_ = "bad local header offset"; return false; }
	const uint8_t* lh = data_ + e.localOffset;
	if (rd32(lh) != 0x04034b50u) { error_ = "bad local header signature"; return false; }
	uint16_t nameLen  = rd16(lh + 26);
	uint16_t extraLen = rd16(lh + 28);
	size_t dataOff = (size_t)e.localOffset + 30 + nameLen + extraLen;
	if (dataOff + e.compressedSize > size_) { error_ = "entry data out of range"; return false; }

	const uint8_t* src = data_ + dataOff;
	if (e.method == 0) {
		out.assign((const char*)src, e.compressedSize);
	} else if (e.method == 8) {
		if (!inflateRaw(src, e.compressedSize, out, e.uncompressedSize)) {
			error_ = "inflate failed for " + e.name;
			return false;
		}
	} else {
		error_ = "unsupported compression method for " + e.name;
		return false;
	}

	if (out.size() != e.uncompressedSize) {
		error_ = "size mismatch for " + e.name;
		return false;
	}
	if (e.crc != 0 &&
	    crc32((const uint8_t*)out.data(), out.size()) != e.crc) {
		error_ = "crc mismatch for " + e.name;
		return false;
	}
	return true;
}

bool Reader::extract(const std::string& name, std::string& out) const
{
	const Entry* e = find(name);
	if (!e) { error_ = "not found: " + name; return false; }
	return extract(*e, out);
}

} // namespace zip
} // namespace appserve
