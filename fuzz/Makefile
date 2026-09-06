# luaext — fuzz targets.
#
# Only PURE modules are fuzzable here: no php.h, no lua.h, no request. That is
# not a limitation of this Makefile, it is the point. A target that needed a
# live PHP would have to be driven through one, and could not be run under
# libFuzzer at all.
#
#   make -f Makefile.dev fuzz              # from the repo root, FUZZ_SECONDS
#   make -C fuzz && ./fuzz/fuzz_vfs_path   # by hand
#
# Requires clang: libFuzzer ships with it and has no gcc equivalent.

CC ?= clang
SRC := ../src

# -fsanitize=fuzzer brings libFuzzer's main(); address,undefined are what turn a
# subtle miscalculation into a stop rather than a wrong answer nobody notices.
CFLAGS ?= -std=gnu11 -g -O1 -Wall -Wextra \
	-fsanitize=fuzzer,address,undefined \
	-fno-omit-frame-pointer \
	-I$(SRC)

TARGETS := fuzz_vfs_path

.PHONY: all clean

all: $(TARGETS)

fuzz_vfs_path: fuzz_vfs_path.c $(SRC)/luaext_vfs_path.c $(SRC)/luaext_vfs_path.h
	@if ! $(CC) --version 2>/dev/null | grep -qi clang; then \
		echo "fuzz: $(CC) is not clang; libFuzzer needs clang. Set CC=clang."; \
		exit 1; \
	fi
	@# Apple's clang answers "clang" and ships no libFuzzer runtime, so the
	@# version check alone sends a macOS contributor into a linker error about
	@# a missing libclang_rt.fuzzer_osx.a. Compiling the flag is the only
	@# honest test for it.
	@if ! printf 'int LLVMFuzzerTestOneInput(const unsigned char *d, unsigned long s){(void)d;(void)s;return 0;}\n' \
		| $(CC) -fsanitize=fuzzer -x c - -o /dev/null 2>/dev/null; then \
		echo "fuzz: $(CC) has no libFuzzer runtime (Apple's clang ships none)."; \
		echo "      brew install llvm, then: make -f Makefile.dev fuzz CC=/opt/homebrew/opt/llvm/bin/clang"; \
		exit 1; \
	fi
	$(CC) $(CFLAGS) -o $@ fuzz_vfs_path.c $(SRC)/luaext_vfs_path.c

clean:
	rm -f $(TARGETS)
	rm -rf $(TARGETS:%=corpus/%)
