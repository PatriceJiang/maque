# basis_universal transcoder（Apache-2.0，见本目录 LICENSE/NOTICE）。
# 只编 KTX2 + Basis Universal 转码所需单文件；供 image 模块解 .ktx2（转 RGBA8）。
add_library(maque_basisu STATIC transcoder/basisu_transcoder.cpp)
add_library(maque::basisu ALIAS maque_basisu)
target_include_directories(maque_basisu PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/transcoder")
target_compile_features(maque_basisu PUBLIC cxx_std_17)
target_compile_definitions(maque_basisu PUBLIC BASISD_SUPPORT_KTX2=1)

# KTX2 的 UASTC Zstd 超压缩需要 libzstd；找不到就关闭该分支（ETC1S 仍可用）。
find_library(ZSTD_LIBRARY NAMES zstd zstd_static libzstd)
find_path(ZSTD_INCLUDE_DIR NAMES zstd.h)
if(ZSTD_LIBRARY AND ZSTD_INCLUDE_DIR)
    target_compile_definitions(maque_basisu PUBLIC BASISD_SUPPORT_KTX2_ZSTD=1)
    target_include_directories(maque_basisu PUBLIC "${ZSTD_INCLUDE_DIR}")
    target_link_libraries(maque_basisu PUBLIC "${ZSTD_LIBRARY}")
    message(STATUS "maque: ktx2 zstd enabled (${ZSTD_LIBRARY})")
else()
    target_compile_definitions(maque_basisu PUBLIC BASISD_SUPPORT_KTX2_ZSTD=0)
    message(WARNING "maque: libzstd 未找到；KTX2 UASTC(Zstd) 不可用，ETC1S 仍可解")
endif()
