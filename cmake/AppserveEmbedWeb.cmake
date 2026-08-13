#---------------------------------------------------------------------------
# appserve_embed_web(<target> WEB_DIR <dir> [NAME <symbol>])
#
# <dir> を zip 化し、その中身を C 配列にした .cpp を生成して <target> に
# リンクする。実行時は WebRoot が最後のフォールバックとして参照する
# (開発中は ./web ディレクトリが優先されるので、埋め込みは配布用)。
#
# exe 末尾追記ではなくリンクにしているのは、プラットフォーム非依存で、
# コード署名を壊さず、CMake だけで完結するため。
#---------------------------------------------------------------------------

set(APPSERVE_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL "appserve cmake dir")

function(appserve_embed_web target)
    cmake_parse_arguments(AEW "" "WEB_DIR;NAME" "" ${ARGN})
    if(NOT AEW_WEB_DIR)
        message(FATAL_ERROR "appserve_embed_web: WEB_DIR is required")
    endif()
    if(NOT AEW_NAME)
        set(AEW_NAME "appserve_embedded_web")
    endif()
    if(NOT IS_DIRECTORY "${AEW_WEB_DIR}")
        message(WARNING "appserve_embed_web: '${AEW_WEB_DIR}' does not exist; skipping")
        return()
    endif()

    set(_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/appserve_embed/${target}")
    set(_zip     "${_gen_dir}/web.zip")
    set(_cpp     "${_gen_dir}/${AEW_NAME}.cpp")
    file(MAKE_DIRECTORY "${_gen_dir}")

    # web/ 配下の全ファイルを依存に取り、変更されたら zip を作り直す
    file(GLOB_RECURSE _web_files
         LIST_DIRECTORIES false
         CONFIGURE_DEPENDS "${AEW_WEB_DIR}/*")

    # zip の中のパスを web/ からの相対にするため、WORKING_DIRECTORY を web/ に
    # 置いたうえで "." を固めず、直下の項目を列挙して渡す。
    file(GLOB _web_top RELATIVE "${AEW_WEB_DIR}" "${AEW_WEB_DIR}/*")

    add_custom_command(
        OUTPUT "${_zip}"
        COMMAND ${CMAKE_COMMAND} -E rm -f "${_zip}"
        COMMAND ${CMAKE_COMMAND} -E tar "cf" "${_zip}" --format=zip -- ${_web_top}
        WORKING_DIRECTORY "${AEW_WEB_DIR}"
        DEPENDS ${_web_files}
        COMMENT "appserve: packing ${AEW_WEB_DIR} -> web.zip"
        VERBATIM)

    add_custom_command(
        OUTPUT "${_cpp}"
        COMMAND ${CMAKE_COMMAND}
                -DINPUT=${_zip}
                -DOUTPUT=${_cpp}
                -DSYMBOL=${AEW_NAME}
                -P "${APPSERVE_CMAKE_DIR}/bin2c.cmake"
        DEPENDS "${_zip}" "${APPSERVE_CMAKE_DIR}/bin2c.cmake"
        COMMENT "appserve: embedding web.zip into ${AEW_NAME}.cpp"
        VERBATIM)

    target_sources(${target} PRIVATE "${_cpp}")
    set_source_files_properties("${_cpp}" PROPERTIES GENERATED TRUE)
endfunction()
