# Makefile - bin2D (MinGW / GCC / Clang)
# 纯 C++17，无第三方依赖（GPU 桥可选：只有 ../binGPU/build-mingw/libbingpu.a 存在时才编）
#
# 目标：
#   all     库 + 全部测试 + 演示（零警告是验收线）
#   test    跑全部测试（失败非 0 退出）
#   demo    跑演示，产出 build-mingw/out/bin2d-demo.png
#   layers  层边界检查：bin2d 里不许出现上层词汇（见规则里的注释）
#   clean

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2 -MMD -MP
LDFLAGS  ?=
# 静态链接 C++ 运行时：产物常在 sh 下被执行（make 的 SHELL=sh），
# Git for Windows 的 /mingw64/bin 可能排在前面命中版本不匹配的 DLL（退出码 127）。
RTLINK   ?= -static-libgcc -static-libstdc++
# Git for Windows 的 sh.exe 路径里有空格：mingw32-make 在 -j N 并行时会把
# 命令拼成 `C:/Program Files/...` 这种未加引号的命令行，CreateProcess 直接失败
# （表现为 process_begin / make (e=2)）。这里用 8.3 短路径绕开空格，
# 且用 `override` 是为了让调用方传 `SHELL='C:/Program Files/Git/bin/sh.exe'` 时
# 也以本文件为准 —— 否则同一份 Makefile 并行/串行行为不一致。
ifeq ($(OS),Windows_NT)
    ifneq ($(wildcard C:/PROGRA~1/Git/bin/sh.exe),)
        override SHELL := C:/PROGRA~1/Git/bin/sh.exe
    endif
endif

SRC_DIR   := src
INC_DIR   := include
BUILD_DIR := build-mingw
OBJ_DIR   := $(BUILD_DIR)/obj
OUT_DIR   := $(BUILD_DIR)/out
TEST_DIR  := $(OUT_DIR)/tests

ifeq ($(OS),Windows_NT)
    EXT := .exe
else
    EXT :=
endif

INC      := -I$(INC_DIR)
LIB_A    := $(BUILD_DIR)/libbin2d.a
LIB_OBJS := $(OBJ_DIR)/raster.o $(OBJ_DIR)/canvas.o $(OBJ_DIR)/scene.o $(OBJ_DIR)/image.o \
            $(OBJ_DIR)/filter.o

# GPU 桥：只有 binGPU 已经构建过（有 libbingpu.a）才进库。
# 没构建时 gpu.cpp 仍做一遍语法检查（gpu-syntax），保证它不会悄悄腐烂。
BINGPU_LIB := $(wildcard ../binGPU/build-mingw/libbingpu.a)
ifeq ($(BINGPU_LIB),)
    GPU_OBJS :=
    GPU_TEST :=
else
    GPU_OBJS := $(OBJ_DIR)/gpu.o $(OBJ_DIR)/display_list.o $(OBJ_DIR)/gpu_exec.o $(OBJ_DIR)/canvas_gpu.o
    GPU_TEST := $(TEST_DIR)/test_gpu_bridge$(EXT) $(TEST_DIR)/test_display_list$(EXT) \
                $(TEST_DIR)/test_content_layer$(EXT)
endif

TESTS := $(TEST_DIR)/test_geometry$(EXT) $(TEST_DIR)/test_raster$(EXT) \
         $(TEST_DIR)/test_composite$(EXT) $(TEST_DIR)/test_scene$(EXT) \
         $(TEST_DIR)/test_filter$(EXT) $(TEST_DIR)/test_image$(EXT) $(TEST_DIR)/test_cache$(EXT) \
         $(TEST_DIR)/test_png$(EXT) $(GPU_TEST)
DEMO  := $(OUT_DIR)/bin2d-demo$(EXT)
BENCH := $(OUT_DIR)/bin2d-bench$(EXT)

# ============================================
# 文字模块（可选静态库 libbin2d_text.a）
#
# 为什么是独立的库：核心库定位"纯 CPU、零第三方依赖"，而字形光栅与整形必须
# FreeType + HarfBuzz。所以核心库的构建路径**不出现**这两个库；只有这个目标
# 才用它们。两个静态库不存在时 `make all` 照常成功（核心 + 现有测试），
# 文字模块整体跳过 —— 这也正是"只想画图形的人不被强制装字体库"的落地方式。
#
# bin2d 自己不带第三方源码树（体积大、不入库），默认指向隔壁 binUI 的
# third_party；换路径用 `make TEXT_DEPS=<dir>`。
# ============================================
TEXT_DEPS   ?= ../binUI/third_party
FT_INC      := $(TEXT_DEPS)/freetype/include
FT_LIB      := $(TEXT_DEPS)/freetype/build/libfreetype.a
HB_INC      := $(TEXT_DEPS)/harfbuzz/src
HB_LIB      := $(TEXT_DEPS)/harfbuzz/build-static/src/libharfbuzz.a
TEXT_SRC    := $(SRC_DIR)/text/text_unicode.cpp $(SRC_DIR)/text/font_face.cpp \
               $(SRC_DIR)/text/font_raster.cpp $(SRC_DIR)/text/font_shaping.cpp \
               $(SRC_DIR)/text/glyph_atlas.cpp
TEXT_OBJS   := $(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(TEXT_SRC))
LIB_TEXT_A  := $(BUILD_DIR)/libbin2d_text.a
TEXT_CFLAGS := -I$(FT_INC) -I$(HB_INC)
TEXT_LIBS   := $(FT_LIB) $(HB_LIB)

# 头 + 静态库都在才算"有依赖"；缺任何一样就整体跳过（不是失败）
TEXT_DEPS_OK := $(and $(wildcard $(FT_LIB)),$(wildcard $(HB_LIB)),\
                     $(wildcard $(FT_INC)/ft2build.h),$(wildcard $(HB_INC)/hb.h))

ifeq ($(TEXT_DEPS_OK),)
TEXT_EXTRA :=
TEXT_TEST  :=
TEXT_NOTE  := "[bin2d] libbin2d_text 跳过：没找到 FreeType/HarfBuzz（$(FT_LIB) / $(HB_LIB)）；核心库与测试照常"
else
TEXT_EXTRA := $(LIB_TEXT_A)
TEXT_TEST  := $(TEST_DIR)/test_text$(EXT)
TEXT_NOTE  := "[bin2d] libbin2d_text 就绪（FreeType + HarfBuzz 只进这个模块）"
endif

.PHONY: all lib test demo bench bench-gpu quality layers clean gpu-syntax text-lib

all: $(LIB_A) $(TEXT_EXTRA) $(TESTS) $(TEXT_TEST) $(DEMO) $(BENCH) $(QUALITY)
	@echo "[bin2d] all: 库 + 测试 + 演示 就绪"
	@echo $(TEXT_NOTE)

lib: $(LIB_A)

# 只编文字模块（验收与"只装字形的人"的入口）
text-lib: $(TEXT_EXTRA)
	@echo $(TEXT_NOTE)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INC) -c $< -o $@

$(LIB_A): $(LIB_OBJS) $(GPU_OBJS)
	@mkdir -p $(dir $@)
	ar rcs $@ $^

# 文字模块：多两个 include 路径（FreeType / HarfBuzz），**只有这里加**
$(OBJ_DIR)/text/%.o: $(SRC_DIR)/text/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INC) $(TEXT_CFLAGS) -c $< -o $@

$(LIB_TEXT_A): $(TEXT_OBJS)
	@mkdir -p $(dir $@)
	ar rcs $@ $^


# GPU 胶水层：绑定 ../binGPU/include/bingpu.h（C 头）
$(OBJ_DIR)/display_list.o: $(SRC_DIR)/display_list.cpp
	$(CXX) $(CXXFLAGS) $(INC) -I../binGPU/include -c $< -o $@

$(OBJ_DIR)/gpu.o: $(SRC_DIR)/gpu.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INC) -I../binGPU/include -c $< -o $@

# GPU 执行体（0.22.0）：和 gpu.cpp 一样只在 binGPU 已构建时进库
$(OBJ_DIR)/canvas_gpu.o: $(SRC_DIR)/canvas_gpu.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INC) -I../binGPU/include -c $< -o $@

$(OBJ_DIR)/gpu_exec.o: $(SRC_DIR)/gpu_exec.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INC) -I../binGPU/include -c $< -o $@

gpu-syntax:
	@echo "[bin2d] gpu.cpp 语法检查（binGPU 未构建时也保证它可编译）"
	@$(CXX) $(CXXFLAGS) $(INC) -I../binGPU/include -fsyntax-only $(SRC_DIR)/gpu.cpp

# ---- 测试 ----
# 测试统一带上 binGPU 的库：libbin2d.a 里现在有 GPU 执行体（canvas_gpu.o → gpu_exec.o），
# 只要测试碰了 Canvas 的 GPU 分支就会被拉进来。没构建 binGPU 时 BINGPU_LIB 为空、
# GPU 对象也不在库里，链接行自然退化成"纯 CPU"那一条。
$(TEST_DIR)/%$(EXT): tests/%.cpp $(LIB_A)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INC) $< $(LIB_A) $(BINGPU_LIB) $(RTLINK) $(LDFLAGS) -o $@

$(TEST_DIR)/test_gpu_bridge$(EXT): tests/test_gpu_bridge.cpp $(LIB_A) $(GPU_OBJS) $(BINGPU_LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INC) -I../binGPU/include $< $(LIB_A) $(BINGPU_LIB) \
	    $(RTLINK) $(LDFLAGS) -o $@

$(TEST_DIR)/test_display_list$(EXT): tests/test_display_list.cpp $(LIB_A) $(GPU_OBJS) $(BINGPU_LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INC) -I../binGPU/include $< $(LIB_A) $(BINGPU_LIB) \
	    $(RTLINK) $(LDFLAGS) -o $@

# 设备侧离屏层（0.22.5）：要 GpuExec 的具体类型（不是窄接口），所以也带 binGPU 的 include
$(TEST_DIR)/test_content_layer$(EXT): tests/test_content_layer.cpp $(LIB_A) $(GPU_OBJS) $(BINGPU_LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INC) -I../binGPU/include $< $(LIB_A) $(BINGPU_LIB) \
	    $(RTLINK) $(LDFLAGS) -o $@

test: $(TESTS) $(TEXT_TEST)
	@fail=0; \
	for t in $(TESTS) $(TEXT_TEST); do \
	    echo "== $$t =="; \
	    ./$$t || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then echo "[bin2d] 测试失败"; exit 1; fi; \
	echo "[bin2d] 全部测试通过"

# 文字模块的用例（只有依赖在时才存在）：彩色字形 / 豆腐块 / 逐字形回退 /
# 多层 text-shadow 四条边界，加图集的两条出口（CPU Image、设备纹理句柄不提）。
$(TEST_DIR)/test_text$(EXT): tests/test_text.cpp $(LIB_TEXT_A) $(LIB_A)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INC) $(TEXT_CFLAGS) $< $(LIB_TEXT_A) $(LIB_A) $(BINGPU_LIB) \
	    $(TEXT_LIBS) $(RTLINK) $(LDFLAGS) -o $@

# ---- 演示 ----
$(DEMO): tools/bin2d_demo.cpp $(LIB_A)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INC) $< $(LIB_A) $(BINGPU_LIB) $(RTLINK) $(LDFLAGS) -o $@

demo: $(DEMO)
	@mkdir -p $(OUT_DIR)
	./$(DEMO) $(OUT_DIR)/bin2d-demo.png
	@echo "[bin2d] 演示图: $(OUT_DIR)/bin2d-demo.png"

# ---- 吞吐基准 ----
$(BENCH): tools/bin2d_bench.cpp $(LIB_A)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INC) $< $(LIB_A) $(BINGPU_LIB) $(RTLINK) $(LDFLAGS) -o $@

bench: $(BENCH)
	@echo "[bin2d] 512x512 目标，六项单线程吞吐（取最好一次）"
	./$(BENCH)

# ---- 图层合成基准（要显卡；只在 binGPU 已构建时存在）----
# 三条路线对比：CPU 合成 / GPU 每帧上传 / GPU 纹理常驻。
# 这条基准是"GPU 到底该不该上"的判据来源（数字进 docs/perf.md）。
ifeq ($(BINGPU_LIB),)
bench-gpu:
	@echo "[bin2d] bench-gpu 跳过：没有 ../binGPU/build-mingw/libbingpu.a"
else
BENCH_GPU := $(OUT_DIR)/bin2d-bench-gpu$(EXT)
$(BENCH_GPU): tools/bin2d_bench_gpu.cpp $(LIB_A) $(BINGPU_LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INC) -I../binGPU/include $< $(LIB_A) $(BINGPU_LIB) \
	    $(RTLINK) $(LDFLAGS) -o $@

bench-gpu: $(BENCH_GPU)
	@echo "[bin2d] 1280x720 目标，16 个图层的三条合成路线"
	./$(BENCH_GPU) auto
endif

# ---- 绘制质量取证（数值 + 图）----
# 不需要显卡也能跑（AA/阶调/缩小/描边）；有 binGPU 时额外跑 GPU 一致性那一项。
QUALITY := $(OUT_DIR)/bin2d-quality$(EXT)
ifeq ($(BINGPU_LIB),)
QUALITY_FLAGS :=
else
QUALITY_FLAGS := -DBIN2D_QUALITY_GPU -I../binGPU/include
endif

$(QUALITY): tools/bin2d_quality.cpp $(LIB_A) $(BINGPU_LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INC) $(QUALITY_FLAGS) $< $(LIB_A) $(BINGPU_LIB) \
	    $(RTLINK) $(LDFLAGS) -o $@

quality: $(QUALITY)
	@mkdir -p $(OUT_DIR)/quality
	./$(QUALITY) $(OUT_DIR)/quality
	@echo "[bin2d] 质量图: $(OUT_DIR)/quality/*.png"

# ---- 层边界 ----
# bin2d 是"上层 UI 库下面"的一层：出现上层概念就说明抽错了。
#   · 不许依赖上层的名字空间/类型（binui::、BinUINode、BINUI_*）；
#   · 不许出现 Node / layout / dom 这些"另一层的事"（-w 只抓独立单词，
#     所以 vertex_layout 这种对方 C 结构体字段名不会被误伤）；
#   · gpu.* 是 binGPU 的胶水层，字段名由对方头文件决定，单独豁免。
#   · **文字模块只能是"被注入"的方向**（2026-11-21 新增）：face 与平台回调由
#     上层注入，模块里出现 binui 任何拼写就算红 —— 字形不出反方向的依赖。
layers:
	@echo "[bin2d] 层边界检查"
	@bad=0; \
	for pat in 'binui::' '#include[ \t]*["<]binui' 'BinUINode' 'BinUIPaintNode' 'BINUI_'; do \
	    hits=$$(grep -rnE "$$pat" $(INC_DIR)/bin2d $(SRC_DIR) || true); \
	    if [ -n "$$hits" ]; then echo "!! 出现上层依赖（$$pat）："; echo "$$hits"; bad=1; fi; \
	done; \
	hits=$$(grep -rnw -e Node -e layout -e dom -e DOM $(INC_DIR)/bin2d $(SRC_DIR) \
	        --exclude='gpu.*' || true); \
	if [ -n "$$hits" ]; then echo "!! 出现上层词汇（Node/layout/dom）："; echo "$$hits"; bad=1; fi; \
	hits=$$(grep -rniE "binui" $(INC_DIR)/bin2d/text.h $(SRC_DIR)/text || true); \
	if [ -n "$$hits" ]; then echo "!! 文字模块反向依赖上层（binui）："; echo "$$hits"; bad=1; fi; \
	if [ $$bad -ne 0 ]; then exit 1; fi; \
	echo "[bin2d] 层边界 OK"

clean:
	rm -rf $(BUILD_DIR)

# 头文件依赖（-MMD 产的 .d）。**必须覆盖每一个 .o**：GPU 桥那四个（gpu / display_list /
# gpu_exec / canvas_gpu）不在 LIB_OBJS 里，以前只补了 gpu.d，于是 display_list.o /
# gpu_exec.o / canvas_gpu.o 改了头也不重建 —— 头里一改结构（比如 Surface 多一个字段），
# 库里的旧目标文件还按老布局读，症状是"测试莫名其妙失败、而且 make 说已经是最新的"。
# 文字模块同理：它的 .o 也不在 LIB_OBJS 里。
-include $(LIB_OBJS:.o=.d) $(GPU_OBJS:.o=.d) $(TEXT_OBJS:.o=.d)


