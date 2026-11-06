#================================================================
#   review/Makefile —— 单文件练习（zmalloc / sds ...）
#
#   用法：make <动作> [模块]       模块省略时默认 zmalloc
#
#   make                    默认编译 zmalloc
#   make build              编译默认模块
#   make build sds          编译 sds
#   make zmalloc            等同 make build zmalloc
#   make run sds            编译并运行 sds
#   make debug sds          用 gdb 调试 sds
#   make asan sds           用 AddressSanitizer 编译并运行 sds
#   make rebuild sds        clean + build（只针对 sds）
#   make clean              删除所有模块的生成物
#   make clean sds          只删除 sds 的生成物
#   make help               显示帮助
#
#   新增模块：在 MODULES 里加名字，并补一条 SRCS_<模块> 即可。
#             多个源文件写成 "a.c b.c"，第一个是入口文件（含 main），
#             其余文件作为库一起链接（它们自带的 demo main 会自动改名）。
#
#   生成物：build/<模块>、build/<模块>.asan，目标文件在 build/obj/<模块>/ 下。
#================================================================

MODULES        := zmalloc sds adlist
DEFAULT_MODULE := zmalloc

# 从 make 的目标列表里挑出模块名（支持 make build sds / make sds build）
REQ_MODULE := $(filter $(MODULES),$(MAKECMDGOALS))
MODULE     ?= $(if $(REQ_MODULE),$(lastword $(REQ_MODULE)),$(DEFAULT_MODULE))

# 每个模块的源文件。需要多个文件就写成 "a.c b.c"。
# 列表里第一个是「入口文件」，它的 main 才是这个程序的 main；
# 靠后的源文件只作为库一起链接，它们自带的 demo main 会被改名（见 stub_main）。
SRCS_zmalloc := zmalloc.c
SRCS_sds     := sds.c zmalloc.c
SRCS_adlist  := adlist.c

SRCS     := $(SRCS_$(MODULE))
MAIN_SRC := $(firstword $(SRCS))

# 非入口文件里的 main 改个名，否则 sds + zmalloc 链接会 multiple definition of `main`
stub_main = $(if $(filter-out $(MAIN_SRC),$(1)),-Dmain=__demo_main_$(basename $(notdir $(1))))

# 未定义的目标直接报错，免得 make build sdz 静默去编 zmalloc
KNOWN_GOALS := all build run debug asan rebuild clean help $(MODULES)
UNKNOWN     := $(filter-out $(KNOWN_GOALS),$(MAKECMDGOALS))
ifneq ($(UNKNOWN),)
$(error 未知目标 '$(firstword $(UNKNOWN))'；可用模块: $(MODULES) | 可用目标: all build run debug asan rebuild clean help)
endif

CC      := gcc
CFLAGS  ?= -g -O0
CFLAGS  += -D _GNU_SOURCE -Wall -Wextra
LDFLAGS ?=
LDLIBS  += -pthread

BUILD_DIR := build
# 目标文件按模块分目录：zmalloc 作为 sds 的依赖时 main 会被改名，
# 和 zmalloc 模块自己要用的 zmalloc.o 不能共用同一个文件
OBJ_DIR   := $(BUILD_DIR)/obj/$(MODULE)
BIN       := $(BUILD_DIR)/$(MODULE)
ASAN_BIN  := $(BUILD_DIR)/$(MODULE).asan
OBJS      := $(SRCS:%.c=$(OBJ_DIR)/%.o)
DEPS      := $(OBJS:.o=.d)
ASAN_OBJS := $(SRCS:%.c=$(OBJ_DIR)/%.asan.o)
ASAN_DEPS := $(ASAN_OBJS:.o=.d)

.PHONY: all build run debug asan rebuild clean help guard $(MODULES)

all: build

# make sds / make zmalloc —— 直接点名模块即为编译它（@: 避免"无需做任何事"的噪音）
# 但如果同时指定了 clean（make clean sds），模块目标就不要再触发编译
ifeq ($(filter clean,$(MAKECMDGOALS)),)
$(MODULES): build
	@:
else
$(MODULES):
	@:
endif

build: guard $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) $(LDLIBS) -o $@

# -MMD -MP 自动生成头文件依赖：改 sds.h / zmalloc.h 都会触发重编
$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(OBJ_DIR)
	$(CC) $(CFLAGS) $(call stub_main,$<) -MMD -MP -c $< -o $@

$(OBJ_DIR)/%.asan.o: %.c
	@mkdir -p $(OBJ_DIR)
	$(CC) $(CFLAGS) -fsanitize=address -fno-omit-frame-pointer $(call stub_main,$<) -MMD -MP -c $< -o $@

-include $(DEPS) $(ASAN_DEPS)

# 源文件为空时给一句人话，而不是让 ld 报 undefined reference to main
guard:
	@if [ -z "$(strip $(SRCS))" ]; then \
		echo "错误: 模块 '$(MODULE)' 没有源文件（检查 SRCS_$(MODULE)）"; exit 1; \
	fi
	@if [ ! -f "$(firstword $(SRCS))" ]; then \
		echo "错误: $(firstword $(SRCS)) 不存在，$(MODULE) 还没有实现内容"; exit 1; \
	fi
	@if [ ! -s "$(firstword $(SRCS))" ]; then \
		echo "错误: $(firstword $(SRCS)) 是空文件，$(MODULE) 还没有实现内容"; exit 1; \
	fi

run: $(BIN)
	@echo "--- run $(MODULE) ---"
	@./$(BIN)

debug: $(BIN)
	gdb -q ./$(BIN)

asan: guard $(ASAN_OBJS)
	$(CC) $(CFLAGS) -fsanitize=address -fno-omit-frame-pointer $(ASAN_OBJS) $(LDFLAGS) $(LDLIBS) -o $(ASAN_BIN)
	@echo "--- run $(MODULE).asan ---"
	@./$(ASAN_BIN)

rebuild: clean build

# make clean         -> 全清
# make clean sds     -> 只清 sds
ifeq ($(REQ_MODULE),)
clean:
	rm -rf $(BUILD_DIR)
else
clean:
	rm -rf $(BIN) $(ASAN_BIN) $(OBJ_DIR)
endif

help:
	@echo "用法: make <动作> [模块]"
	@echo ""
	@echo "模块: $(MODULES)    (默认 $(DEFAULT_MODULE))"
	@echo ""
	@echo "  build    编译        make build sds"
	@echo "  run      编译并运行  make run sds"
	@echo "  debug    gdb 调试    make debug sds"
	@echo "  asan     ASan 运行   make asan sds"
	@echo "  rebuild  clean + build"
	@echo "  clean    删除生成物（不带模块则全清）"
	@echo ""
	@echo "生成物都在 $(BUILD_DIR)/ 下，不会污染源码目录"
