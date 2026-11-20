cmake_minimum_required(VERSION 3.16)
project(bin2D VERSION 0.20.0 LANGUAGES CXX)

# bin2d 是纯 C++17 的 2D 绘制核：无第三方依赖。
# GPU 桥（src/gpu.cpp）可选 —— 只有定义了 BIN2D_BINGPU_DIR 且能找到 bingpu.h 时才编。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_library(bin2d STATIC
    src/raster.cpp
    src/canvas.cpp
    src/scene.cpp
    src/image.cpp
)
target_include_directories(bin2d PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if (MSVC)
    target_compile_options(bin2d PRIVATE /W4 /permissive-)
else()
    target_compile_options(bin2d PRIVATE -Wall -Wextra)
endif()

# ---- 测试 ----
enable_testing()
set(BIN2D_TESTS test_geometry test_raster test_composite test_scene test_png)
foreach(t ${BIN2D_TESTS})
    add_executable(${t} tests/${t}.cpp)
    target_link_libraries(${t} PRIVATE bin2d)
    target_include_directories(${t} PRIVATE tests)
    add_test(NAME ${t} COMMAND ${t})
endforeach()

# ---- 绘制质量取证（数值 + 图，不需要显卡）----
add_executable(bin2d-quality tools/bin2d_quality.cpp)
target_link_libraries(bin2d-quality PRIVATE bin2d)

# ---- 演示 ----
add_executable(bin2d-demo tools/bin2d_demo.cpp)
target_link_libraries(bin2d-demo PRIVATE bin2d)
set_target_properties(bin2d-demo PROPERTIES OUTPUT_NAME bin2d-demo)

# ---- 可选：binGPU 桥 ----
set(BIN2D_BINGPU_DIR "" CACHE PATH "binGPU 目录（含 include/bingpu.h 与 libbingpu.a）")
if (BIN2D_BINGPU_DIR AND EXISTS "${BIN2D_BINGPU_DIR}/include/bingpu.h")
    target_sources(bin2d PRIVATE src/gpu.cpp)
    target_include_directories(bin2d PUBLIC ${BIN2D_BINGPU_DIR}/include)
    if (EXISTS "${BIN2D_BINGPU_DIR}/build-mingw/libbingpu.a")
        target_link_libraries(bin2d PUBLIC "${BIN2D_BINGPU_DIR}/build-mingw/libbingpu.a")
        add_executable(test_gpu_bridge tests/test_gpu_bridge.cpp)
        target_link_libraries(test_gpu_bridge PRIVATE bin2d)
        target_include_directories(test_gpu_bridge PRIVATE tests ${BIN2D_BINGPU_DIR}/include)
        add_test(NAME test_gpu_bridge COMMAND test_gpu_bridge)
        add_executable(bin2d-bench-gpu tools/bin2d_bench_gpu.cpp)
        target_link_libraries(bin2d-bench-gpu PRIVATE bin2d)
        target_compile_definitions(bin2d-quality PRIVATE BIN2D_QUALITY_GPU)
    endif()
endif()

# ---- 可选：文字模块（libbin2d_text，0.22.7）----
# 字形光栅 / 整形 / 图集 / 文字阴影。**核心库不链它**（"纯 CPU、零第三方依赖"那条
# 不变）；只有要画字的调用方链 libbin2d_text.a + FreeType + HarfBuzz。
# 与 Makefile 的 `text-lib` 目标同一条判据：依赖在才编，缺了核心库照常过。
set(BIN2D_TEXT_DEPS "${CMAKE_CURRENT_SOURCE_DIR}/../binUI/third_party"
    CACHE PATH "FreeType/HarfBuzz 的 third_party 目录（含 freetype/ 与 harfbuzz/）")
set(BIN2D_FT_DIR "${BIN2D_TEXT_DEPS}/freetype")
set(BIN2D_HB_DIR "${BIN2D_TEXT_DEPS}/harfbuzz")
if(EXISTS "${BIN2D_FT_DIR}/include/ft2build.h" AND EXISTS "${BIN2D_HB_DIR}/src/hb.h")
    if(NOT TARGET freetype)
        add_subdirectory("${BIN2D_FT_DIR}" EXCLUDE_FROM_ALL)
    endif()
    if(NOT TARGET harfbuzz)
        add_subdirectory("${BIN2D_HB_DIR}" EXCLUDE_FROM_ALL)
    endif()
    add_library(bin2d_text STATIC
        src/text/text_unicode.cpp
        src/text/font_face.cpp
        src/text/font_raster.cpp
        src/text/font_shaping.cpp
        src/text/glyph_atlas.cpp)
    target_link_libraries(bin2d_text PUBLIC bin2d freetype harfbuzz)
    target_include_directories(bin2d_text PRIVATE
        "${BIN2D_FT_DIR}/include" "${BIN2D_HB_DIR}/src")
    add_executable(test_text tests/test_text.cpp)
    target_link_libraries(test_text PRIVATE bin2d_text)
    add_test(NAME test_text COMMAND test_text)
    install(TARGETS bin2d_text ARCHIVE DESTINATION lib)
    install(FILES include/bin2d/text.h DESTINATION include/bin2d)
else()
    message(STATUS "bin2d: text module skipped (no FreeType/HarfBuzz under ${BIN2D_TEXT_DEPS})")
endif()

install(TARGETS bin2d ARCHIVE DESTINATION lib)
install(DIRECTORY include/bin2d DESTINATION include)
install(FILES include/bin2d/paint.h include/bin2d/path.h include/bin2d/raster.h
              include/bin2d/canvas.h include/bin2d/scene.h include/bin2d/image.h
              include/bin2d/gpu.h
        DESTINATION include/bin2d)

