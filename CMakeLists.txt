#---------------------------------------------------------------------------
# appserve - ブラウザを UI とするローカルアプリのためのミニマム C++ フレームワーク
#
#   派生アプリからの利用:
#     add_subdirectory(external/appserve)
#     target_link_libraries(myapp PRIVATE appserve::core)
#     appserve_embed_web(myapp WEB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/web)
#---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.16)

# トップレベルビルドのときだけ project() を宣言する (add_subdirectory 取り込みを壊さない)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(appserve LANGUAGES CXX)
    set(APPSERVE_IS_TOP_LEVEL ON)
else()
    set(APPSERVE_IS_TOP_LEVEL OFF)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(APPSERVE_BUILD_APP   "Build the bundled base application (appserve executable)" ${APPSERVE_IS_TOP_LEVEL})
option(APPSERVE_BUILD_TESTS "Build tests" OFF)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/AppserveEmbedWeb.cmake)

#---------------------------------------------------------------------------
# appserve::core
#
# 依存は「標準ライブラリ + OS ソケット」だけ。zip の読み出しも自前
# (src/webroot/zip.cpp) なので、vcpkg も FetchContent も不要でビルドできる。
#---------------------------------------------------------------------------
add_library(appserve_core STATIC
    src/json/json.cpp
    src/core/log.cpp
    src/core/options.cpp
    src/core/taskqueue.cpp
    src/core/session.cpp
    src/core/util.cpp
    src/core/app.cpp
    src/http/socket.cpp
    src/http/parse.cpp
    src/http/mime.cpp
    src/http/server.cpp
    src/webroot/webroot.cpp
    src/webroot/zip.cpp
    src/browser/launcher.cpp
    src/browser/channel.cpp
    src/repl/repl.cpp
    src/repl/commands.cpp
    src/repl/stdin_channel.cpp
    src/repl/file_channel.cpp
    src/plugin/registry.cpp
    src/plugin/loader.cpp
    modules/fs/fs_module.cpp
)
add_library(appserve::core ALIAS appserve_core)

target_include_directories(appserve_core
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
set_target_properties(appserve_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)
target_link_libraries(appserve_core PUBLIC Threads::Threads)

if(WIN32)
    target_link_libraries(appserve_core PUBLIC ws2_32 shell32)
    target_compile_definitions(appserve_core PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
else()
    target_link_libraries(appserve_core PUBLIC ${CMAKE_DL_LIBS})
endif()

if(MSVC)
    target_compile_options(appserve_core PUBLIC /utf-8 /Zc:__cplusplus)
    target_compile_options(appserve_core PRIVATE /W3)
else()
    target_compile_options(appserve_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

#---------------------------------------------------------------------------
# ベースアプリ (ローカルファイルアクセス + ファイルブラウザ UI)
#---------------------------------------------------------------------------
if(APPSERVE_BUILD_APP)
    add_executable(appserve app/main.cpp)
    target_link_libraries(appserve PRIVATE appserve::core)
    # web/ を zip 化して実行ファイルへ埋め込む。開発中はカレント/exe 隣の
    # web/ ディレクトリが優先されるので、埋め込みはリリース時のフォールバック。
    appserve_embed_web(appserve WEB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/web)
endif()

if(APPSERVE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
