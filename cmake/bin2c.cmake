#---------------------------------------------------------------------------
# バイナリファイルを C++ 配列にする (cmake -P で実行するスクリプトモード)
#
#   cmake -DINPUT=web.zip -DOUTPUT=embed.cpp -DSYMBOL=appserve_embedded_web
#         -P bin2c.cmake
#
# 外部ツール (xxd 等) に依存しないよう CMake だけで完結させる。
# 生成物は静的初期化で WebRoot::registerEmbedded() を呼ぶ。
#---------------------------------------------------------------------------
if(NOT INPUT OR NOT OUTPUT OR NOT SYMBOL)
    message(FATAL_ERROR "bin2c: INPUT, OUTPUT and SYMBOL are required")
endif()

file(READ "${INPUT}" _hex HEX)
string(LENGTH "${_hex}" _hexlen)
math(EXPR _bytes "${_hexlen} / 2")

# "0011aa" -> "0x00,0x11,0xaa," に変換し、16 バイトごとに改行を入れる
string(REGEX REPLACE "(..)" "0x\\1," _body "${_hex}")
string(REGEX REPLACE "((0x..,){16})" "\\1\n\t" _body "${_body}")

file(WRITE "${OUTPUT}"
"//---------------------------------------------------------------------------\n"
"// 自動生成 — appserve_embed_web() が ${INPUT} から生成しました。編集しないこと。\n"
"//---------------------------------------------------------------------------\n"
"#include <cstddef>\n"
"\n"
"// src/webroot/webroot.h の宣言と対。ここではヘッダを include せずに済ませる。\n"
"namespace appserve {\n"
"void appserveRegisterEmbeddedWeb(const unsigned char* data, size_t size);\n"
"} // namespace appserve\n"
"\n"
"namespace {\n"
"\n"
"const unsigned char k_${SYMBOL}[] = {\n"
"\t${_body}\n"
"};\n"
"\n"
"// 静的初期化で登録する。main より前に走るので、App::run() の時点で参照できる。\n"
"struct ${SYMBOL}_registrar {\n"
"\t${SYMBOL}_registrar() {\n"
"\t\tappserve::appserveRegisterEmbeddedWeb(k_${SYMBOL}, ${_bytes});\n"
"\t}\n"
"};\n"
"const ${SYMBOL}_registrar g_${SYMBOL}_registrar;\n"
"\n"
"} // anonymous\n")

message(STATUS "bin2c: ${INPUT} -> ${OUTPUT} (${_bytes} bytes)")
