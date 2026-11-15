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
LIB_OBJS := $(OBJ_DIR)/raster.o $(OBJ_DIR)/canvas.o $(OBJ_DIR)/scene.o $(OBJ_DIR)/image.o

# GPU 桥：只有 binGPU 已经构建过（有 libbingpu.a）才进库。
# 没构建时 gpu.cpp 仍做一遍语法检查（gpu-syntax），保证它不会悄悄腐烂。
BINGPU_LIB := $(wildcard ../binGPU/build-mingw/libbingpu.a)
ifeq ($(BINGPU_LIB),)
    GPU_OBJS :=
    GPU_TEST :=
else
    GPU_OBJS := $(OBJ_DIR)/gpu.o
    GPU_TEST := $(TEST_DIR)/test_gpu_bridge$(EXT)
endif

TESTS := $(TEST_DIR)/test_geometry$(EXT) $(TEST_DIR)/test_raster$(EXT) \
         $(TEST_DIR)/test_composite$(EXT) $(TEST_DIR)/test_scene$(EXT) \
         $(TEST_DIR)/test_png$(EXT) $(GPU_TEST)
DEMO  := $(OUT_DIR)/bin2d-demo$(EXT)

.PHONY: all lib test demo layers clean gpu-syntax

all: $(LIB_A) $(TESTS) $(DEMO)
	@echo "[bin2d] all: 库 + 测试 + 演示 就绪"

lib: $(LIB_A)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INC) -c $< -o $@

$(LIB_A): $(LIB_OBJS) $(GPU_OBJS)
	@mkdir -p $(dir $@)
	ar rcs $@ $^

# GPU 胶水层：绑定 ../binGPU/include/bingpu.h（C 头）
$(OBJ_DIR)/gpu.o: $(SRC_DIR)/gpu.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INC) -I../binGPU/include -c $< -o $@

gpu-syntax:
	@echo "[bin2d] gpu.cpp 语法检查（binGPU 未构建时也保证它可编译）"
	@$(CXX) $(CXXFLAGS) $(INC) -I../binGPU/include -fsyntax-only $(SRC_DIR)/gpu.cpp

# ---- 测试 ----
$(TEST_DIR)/%$(EXT): tests/%.cpp $(LIB_A)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INC) $< $(LIB_A) $(RTLINK) $(LDFLAGS) -o $@

$(TEST_DIR)/test_gpu_bridge$(EXT): tests/test_gpu_bridge.cpp $(LIB_A) $(GPU_OBJS) $(BINGPU_LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INC) -I../binGPU/include $< $(LIB_A) $(BINGPU_LIB) \
	    $(RTLINK) $(LDFLAGS) -o $@

test: $(TESTS)
	@fail=0; \
	for t in $(TESTS); do \
	    echo "== $$t =="; \
	    ./$$t || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then echo "[bin2d] 测试失败"; exit 1; fi; \
	echo "[bin2d] 全部测试通过"

# ---- 演示 ----
$(DEMO): tools/bin2d_demo.cpp $(LIB_A)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INC) $< $(LIB_A) $(RTLINK) $(LDFLAGS) -o $@

demo: $(DEMO)
	@mkdir -p $(OUT_DIR)
	./$(DEMO) $(OUT_DIR)/bin2d-demo.png
	@echo "[bin2d] 演示图: $(OUT_DIR)/bin2d-demo.png"

# ---- 层边界 ----
# bin2d 是"上层 UI 库下面"的一层：出现上层概念就说明抽错了。
#   · 不许依赖上层的名字空间/类型（binui::、BinUINode、BINUI_*）；
#   · 不许出现 Node / layout / dom 这些"另一层的事"（-w 只抓独立单词，
#     所以 vertex_layout 这种对方 C 结构体字段名不会被误伤）；
#   · gpu.* 是 binGPU 的胶水层，字段名由对方头文件决定，单独豁免。
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
	if [ $$bad -ne 0 ]; then exit 1; fi; \
	echo "[bin2d] 层边界 OK"

clean:
	rm -rf $(BUILD_DIR)

-include $(LIB_OBJS:.o=.d) $(OBJ_DIR)/gpu.d
