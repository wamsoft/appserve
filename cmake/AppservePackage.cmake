#---------------------------------------------------------------------------
# appserve_package(<target>
#     [NAME <package-name>]        既定: ターゲット名
#     [VERSION <x.y.z>]            既定: PROJECT_VERSION か 0.0.0
#     [VENDOR <name>]              既定: wamsoft
#     [DESCRIPTION <text>]
#     [LICENSE <file>]             既定: LICENSE があればそれ
#     [EXTRA_FILES <f> ...]        同梱したい追加ファイル (README 等)
#     [NO_INSTALLER]               Windows でもインストーラを作らない
# )
#
# appserve ベースのアプリは「UI 埋め込み済みの exe 1 つ」で動くので、配布物も
# 実行ファイル + README + LICENSE だけで済む。この関数は install() と CPack を
# その前提で設定する。
#
# 生成される成果物:
#   <name>-<version>-<os>-<arch>.zip        全プラットフォーム (ポータブル版)
#   <name>-<version>-<os>-<arch>.exe        Windows + NSIS があるとき (インストーラ)
#   <name>-<version>-<os>-<arch>.tar.gz     Windows 以外
#
# 使い方:
#   appserve_embed_web(myapp WEB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/web)
#   appserve_package(myapp VERSION 1.2.3 DESCRIPTION "My tool")
#   → cmake --build build --config Release
#     cpack --config build/CPackConfig.cmake -C Release
#---------------------------------------------------------------------------

function(appserve_package target)
    cmake_parse_arguments(AP "NO_INSTALLER"
        "NAME;VERSION;VENDOR;DESCRIPTION;LICENSE" "EXTRA_FILES" ${ARGN})

    if(NOT TARGET ${target})
        message(FATAL_ERROR "appserve_package: '${target}' is not a target")
    endif()
    if(NOT AP_NAME)
        set(AP_NAME "${target}")
    endif()
    if(NOT AP_VENDOR)
        set(AP_VENDOR "wamsoft")
    endif()
    # "0" のようなバージョンを CMake の偽値と取り違えないよう、空判定で書く
    if(AP_VERSION STREQUAL "")
        if(NOT PROJECT_VERSION STREQUAL "")
            set(AP_VERSION "${PROJECT_VERSION}")
        else()
            set(AP_VERSION "0.0.0")
        endif()
    endif()
    if(NOT AP_DESCRIPTION)
        set(AP_DESCRIPTION "${AP_NAME}")
    endif()
    if(NOT AP_LICENSE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
        set(AP_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
    endif()

    # --- 中身 -------------------------------------------------------------
    # UI は exe に埋め込まれているので、実行ファイル 1 つだけをフラットに置く。
    # (階層を作らないのは、zip を展開してそのまま起動できるようにするため)
    install(TARGETS ${target} RUNTIME DESTINATION . BUNDLE DESTINATION .)

    set(_docs ${AP_EXTRA_FILES})
    foreach(_f README.md CHANGELOG.md)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_f}")
            list(APPEND _docs "${CMAKE_CURRENT_SOURCE_DIR}/${_f}")
        endif()
    endforeach()
    if(AP_LICENSE)
        list(APPEND _docs "${AP_LICENSE}")
    endif()
    if(_docs)
        install(FILES ${_docs} DESTINATION .)
    endif()

    # --- パッケージ名 -----------------------------------------------------
    # <name>-<version>-<os>-<arch> に揃える。GitHub Release で複数 OS の
    # 成果物が並んだときに、どれを落とせばよいか一目で分かるようにする。
    if(WIN32)
        set(_os "windows")
    elseif(APPLE)
        set(_os "macos")
    else()
        set(_os "linux")
    endif()

    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_arch "x64")
    else()
        set(_arch "x86")
    endif()
    # Apple Silicon / ARM クロスビルドを拾う
    if(CMAKE_OSX_ARCHITECTURES MATCHES "arm64" OR
       CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
        set(_arch "arm64")
    endif()

    set(CPACK_PACKAGE_NAME              "${AP_NAME}")
    set(CPACK_PACKAGE_VENDOR            "${AP_VENDOR}")
    set(CPACK_PACKAGE_VERSION           "${AP_VERSION}")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${AP_DESCRIPTION}")
    set(CPACK_PACKAGE_FILE_NAME         "${AP_NAME}-${AP_VERSION}-${_os}-${_arch}")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "${AP_NAME}")
    set(CPACK_PACKAGE_EXECUTABLES       "${target}" "${AP_NAME}")
    set(CPACK_STRIP_FILES               TRUE)
    set(CPACK_VERBATIM_VARIABLES        TRUE)
    if(AP_LICENSE)
        set(CPACK_RESOURCE_FILE_LICENSE "${AP_LICENSE}")
    endif()

    # zip はディレクトリを 1 段作る (展開時にカレントを散らかさない)
    set(CPACK_ARCHIVE_COMPONENT_INSTALL OFF)
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY 1)

    # --- ジェネレータ -----------------------------------------------------
    if(WIN32)
        set(_gen "ZIP")
        if(NOT AP_NO_INSTALLER)
            # NSIS が無い環境 (素の開発機など) では zip だけにフォールバックする。
            # CI では workflow 側で NSIS を入れてインストーラも作る。
            find_program(APPSERVE_MAKENSIS makensis
                PATHS "$ENV{ProgramFiles\(x86\)}/NSIS" "$ENV{ProgramFiles}/NSIS")
            if(APPSERVE_MAKENSIS)
                list(APPEND _gen "NSIS")
                set(CPACK_NSIS_PACKAGE_NAME       "${AP_NAME}")
                set(CPACK_NSIS_DISPLAY_NAME       "${AP_NAME} ${AP_VERSION}")
                set(CPACK_NSIS_INSTALLED_ICON_NAME "${target}.exe")
                set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
                set(CPACK_NSIS_MODIFY_PATH        OFF)
                # スタートメニューにショートカットを置く
                set(CPACK_NSIS_CREATE_ICONS_EXTRA
                    "CreateShortCut '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\${AP_NAME}.lnk' '$INSTDIR\\\\${target}.exe'")
                set(CPACK_NSIS_DELETE_ICONS_EXTRA
                    "Delete '$SMPROGRAMS\\\\$START_MENU\\\\${AP_NAME}.lnk'")
            else()
                message(STATUS
                    "appserve_package: makensis not found — packaging ZIP only")
            endif()
        endif()
    elseif(APPLE)
        set(_gen "ZIP" "TGZ")
    else()
        set(_gen "TGZ" "ZIP")
    endif()
    set(CPACK_GENERATOR ${_gen})

    # CPack は include() したディレクトリスコープの変数を読むので、
    # 関数スコープから親へ引き上げてから include する。
    get_cmake_property(_vars VARIABLES)
    foreach(_v ${_vars})
        if(_v MATCHES "^CPACK_")
            set(${_v} "${${_v}}" PARENT_SCOPE)
        endif()
    endforeach()
    set(APPSERVE_PACKAGE_FILE_NAME "${CPACK_PACKAGE_FILE_NAME}" PARENT_SCOPE)
    set(APPSERVE_PACKAGE_GENERATORS "${_gen}" PARENT_SCOPE)
endfunction()

#---------------------------------------------------------------------------
# appserve_package() を呼んだ後に 1 回だけ呼ぶ (include(CPack) の実行)。
# CPack はディレクトリスコープで動くので、関数の中からは include できない。
#---------------------------------------------------------------------------
macro(appserve_package_finalize)
    include(CPack)
endmacro()
