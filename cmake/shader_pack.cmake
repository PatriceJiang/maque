# 内建 shader 打包流水线（docs/13）：assets/builtin/shaders/<name>/*.wgsl → 构建目录里的压缩源码。
#
# 资产是唯一真相（随仓库提交）：一个 shader 一个目录，**目录名 = shader 名**，目录内 *.wgsl
# 按文件名排序拼接。流水线在**配置期**生成（与 core/data 的 codegen 同风格，生成物不入库）：
#   <out>/<name>.cpp          每个 shader 一个压缩源码文件（zstd Blob + 取源码的函数）
#   <out>/builtin_shaders.h   注册表类型 / 表 / count / mq_builtin_shader_text()
#   <out>/builtin_shaders.cpp 注册表（只含**编入**的 shader）
#
# **可剔除**：每个 shader 一个缓存开关 MAQUE_SHADER_<NAME>（默认 ON）：
#   cmake -S . -B build -DMAQUE_SHADER_TEXTURED=OFF      # 不编入 textured（不生成、不链接、不进二进制）
# 压缩：MAQUE_BUILTIN_SHADER_COMPRESS（默认 ON）需要 libzstd（运行时解压）+ node（打包，zstd 由
# node zlib 或 zstd CLI 提供）；缺 libzstd 时自动退回直嵌原文（功能不变，仅体积大）。
#
# 用法（模块 CMakeLists）：
#   include(cmake/shader_pack.cmake)
#   maque_builtin_shaders("${CMAKE_SOURCE_DIR}/assets/builtin/shaders"
#                         "${CMAKE_BINARY_DIR}/generated/builtin_shaders"
#                         SHADER_SOURCES SHADER_OUT_DIR)
#   → 生成目标 maque_builtin_shaders（静态库，含解压实现）+ 变量 SHADER_SOURCES / SHADER_OUT_DIR
#     + 手动重打包目标 `maque_shaders`。

include_guard(GLOBAL)

find_program(NODE_EXECUTABLE node)
option(MAQUE_BUILTIN_SHADER_COMPRESS "内建 shader 用 zstd 压缩（运行时由 libzstd 解压）" ON)

# 解压实现（手写、非生成物）：目前只有 gfx 消费内建 shader，故直接指向 gfx 的源文件。
set(MAQUE_SHADER_DECOMPRESS_SRC "${CMAKE_SOURCE_DIR}/modules/render/gfx/src/shader_decompress.cpp")

function(maque_builtin_shaders assets_dir out_dir sources_var include_var)
    set(packer "${CMAKE_SOURCE_DIR}/tools/shader_pack.js")
    if(NOT EXISTS "${assets_dir}")
        message(WARNING "maque: 内建 shader 资产目录不存在（跳过打包）: ${assets_dir}")
        set(${sources_var} "" PARENT_SCOPE)
        set(${include_var} "${out_dir}" PARENT_SCOPE)
        return()
    endif()
    if(NOT NODE_EXECUTABLE)
        message(FATAL_ERROR "maque: 内建 shader 打包需要 node（未找到）")
    endif()

    # ---- 发现 shader（一级子目录；目录名 = shader 名）+ 每个一个剔除开关（默认编入）----
    file(GLOB shader_candidates LIST_DIRECTORIES true RELATIVE "${assets_dir}" "${assets_dir}/*")
    set(enabled "")
    set(assets "")
    foreach(entry IN LISTS shader_candidates)
        if(NOT IS_DIRECTORY "${assets_dir}/${entry}")
            continue()
        endif()
        file(GLOB_RECURSE shader_files CONFIGURE_DEPENDS "${assets_dir}/${entry}/*.wgsl")
        if(NOT shader_files)
            continue()                                   # 目录里没有 .wgsl：不算 shader（如 README）
        endif()
        string(TOUPPER "${entry}" upper)
        option(MAQUE_SHADER_${upper} "编入内建 shader '${entry}'（关掉即剔除）" ON)
        if(MAQUE_SHADER_${upper})
            list(APPEND enabled "${entry}")
            list(APPEND assets ${shader_files})
        endif()
    endforeach()

    # ---- 压缩：需要 libzstd（运行时解压）----
    set(compress_flag "")
    set(zstd_lib "")
    set(zstd_inc "")
    if(MAQUE_BUILTIN_SHADER_COMPRESS)
        # 库名要覆盖各平台的静态/动态变体：
        #   Linux   libzstd.so / libzstd.a
        #   Windows zstd.lib（动态导入库）/ zstd_static.lib（vcpkg 与自建都常见）
        find_library(ZSTD_LIBRARY NAMES zstd zstd_static libzstd)
        find_path(ZSTD_INCLUDE_DIR NAMES zstd.h)
        if(ZSTD_LIBRARY AND ZSTD_INCLUDE_DIR)
            set(zstd_lib "${ZSTD_LIBRARY}")
            set(zstd_inc "${ZSTD_INCLUDE_DIR}")
        else()
            if(ZSTD_LIBRARY AND NOT ZSTD_INCLUDE_DIR)
                message(WARNING "maque: 找到 libzstd 但缺少 zstd.h（ZSTD_INCLUDE_DIR 未命中）→ 内建 shader 直嵌原文")
            else()
                message(WARNING "maque: 未找到 libzstd → 内建 shader 直嵌原文"
                                "（安装 libzstd-dev / Windows 用 tools/win/fetch-deps.ps1，"
                                "或显式 -DMAQUE_BUILTIN_SHADER_COMPRESS=OFF）")
            endif()
            set(zstd_lib "")
        endif()
    else()
        set(compress_flag "--no-compress")
    endif()

    # ---- 打包（配置期；改资产会自动重新配置 → 重新打包）----
    set(gen_dir "${out_dir}")
    file(MAKE_DIRECTORY "${gen_dir}")
    string(REPLACE ";" "," enabled_csv "${enabled}")
    execute_process(
        COMMAND "${NODE_EXECUTABLE}" "${packer}" --assets "${assets_dir}" --out "${gen_dir}"
                --shaders "${enabled_csv}" ${compress_flag}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE pack_rc
        OUTPUT_VARIABLE pack_out
        ERROR_VARIABLE pack_err)
    if(NOT pack_rc EQUAL 0)
        message(FATAL_ERROR "maque: 内建 shader 打包失败（node tools/shader_pack.js 返回 ${pack_rc}）\n${pack_err}${pack_out}")
    endif()
    string(STRIP "${pack_out}" pack_out)
    if(pack_out)
        message(STATUS "${pack_out}")
    endif()

    # 生成物清单：注册表 + 每个编入的 shader + 解压实现（手写）
    set(gen_sources "${gen_dir}/builtin_shaders.cpp" "${MAQUE_SHADER_DECOMPRESS_SRC}")
    foreach(name IN LISTS enabled)
        list(APPEND gen_sources "${gen_dir}/${name}.cpp")
    endforeach()

    add_library(maque_builtin_shaders STATIC ${gen_sources})
    add_library(maque::builtin_shaders ALIAS maque_builtin_shaders)
    target_include_directories(maque_builtin_shaders PUBLIC "${gen_dir}")
    target_include_directories(maque_builtin_shaders PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    # 编入的 shader 数量（单测据此断言注册表内容与启用开关一致；0 也是合法配置）
    list(LENGTH enabled shader_count)
    set_property(TARGET maque_builtin_shaders PROPERTY MAQUE_SHADER_COUNT "${shader_count}")
    set_target_properties(maque_builtin_shaders PROPERTIES
        CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON
        POSITION_INDEPENDENT_CODE ON)          # 同时被 .so（gfx 插件）与可执行（测试）链接
    if(zstd_lib)
        target_link_libraries(maque_builtin_shaders PUBLIC "${zstd_lib}")
        target_compile_definitions(maque_builtin_shaders PRIVATE MQ_HAVE_ZSTD)
        if(zstd_inc)
            # PUBLIC：链接进 gfx 插件/测试的目标同样需要 zstd.h（shader_decompress.cpp 在其中）
            target_include_directories(maque_builtin_shaders PUBLIC "${zstd_inc}")
        endif()
    endif()

    # 资产内容变化也必须触发重打包：CONFIGURE_DEPENDS 的 glob 校验只发现文件增删，
    # 不会发现已有 .wgsl 被修改。用 stamp 显式跟踪资产与打包器，避免二进制继续内嵌旧 shader
    # （曾导致磁盘源码已修文字吸附、播放器仍复现旧基线/字距问题）。
    set(pack_stamp "${gen_dir}/maque_shaders.stamp")
    set(pack_outputs "${pack_stamp}" "${gen_dir}/builtin_shaders.cpp" "${gen_dir}/builtin_shaders.h")
    foreach(name IN LISTS enabled)
        list(APPEND pack_outputs "${gen_dir}/${name}.cpp")
    endforeach()
    add_custom_command(
        OUTPUT ${pack_outputs}
        COMMAND "${NODE_EXECUTABLE}" "${packer}" --assets "${assets_dir}" --out "${gen_dir}"
                --shaders "${enabled_csv}" ${compress_flag}
        COMMAND "${CMAKE_COMMAND}" -E touch "${pack_stamp}"
        DEPENDS ${assets} "${packer}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "maque: 重新打包内建 shader（assets/builtin/shaders → 压缩源码）"
        VERBATIM)
    add_custom_target(maque_shaders DEPENDS ${pack_outputs})
    add_dependencies(maque_builtin_shaders maque_shaders)

    # 资产变化 → 重新配置（从而重新打包）
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${assets} "${assets_dir}")

    set(${sources_var} "${gen_sources}" PARENT_SCOPE)
    set(${include_var} "${gen_dir}" PARENT_SCOPE)
endfunction()
