#!/usr/bin/env bash
#
# What the vendored patches cost, measured against stock Lua 5.5.1.
#
# Builds third_party/lua-5.5.1/src/ TWICE from the same files, the same
# compiler and the same flags, differing only in LUAEXT_LUA_HOOKS:
#
#   0  every patch compiles out -- this is upstream 5.5.1, byte for byte
#   1  the interpreter luaext actually ships
#
# so the number it prints is the patches and nothing else. Timing a system
# `lua` binary instead would measure that distribution's compiler flags at
# least as much as it measured us.
#
# Usage:
#   tools/bench-vm.sh [runs]        # default 5, best-of
#
# This measures the INTERPRETER only: no PHP boundary, no conversion, no
# callback bridge, no watchdog thread. That is deliberate -- it is the only
# figure comparable to a Lua author's own benchmark, and the only one worth
# quoting as "what the sandbox costs to run Lua".

set -euo pipefail

runs="${1:-5}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
src="$root/third_party/lua-5.5.1/src"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

if [ ! -f "$root/bench/vm-bench.c" ]; then
	echo "bench-vm: bench/vm-bench.c is missing" >&2
	exit 1
fi

# Every translation unit the manifest lists, minus nothing: the same set the
# extension links, so the interpreter under test is the interpreter that ships.
mapfile -t sources < <(grep -vE '^\s*(#|$)' "$root/third_party/lua-5.5.1/SOURCES")

cc="${CC:-cc}"
# -O2 because that is what a release build of the extension uses; a -O0
# comparison would flatter the patched build by drowning it in dispatch cost.
flags=(-O2 -std=gnu11 -w -I"$src")

build() {
	local hooks="$1"
	local hook_mode="$2"
	local out="$3"
	# Declared separately: bash expands the whole `local` line before assigning,
	# so a later name referring to an earlier one on the same line reads empty.
	local object_dir="$work/obj$hooks$hook_mode"
	local objects=()
	local defines=(-DLUAEXT_LUA_HOOKS="$hooks" -DLUAEXT_BENCH_HOOK="$hook_mode")

	mkdir -p "$object_dir"

	for file in "${sources[@]}"; do
		"$cc" "${flags[@]}" "${defines[@]}" \
			-c "$src/$file" -o "$object_dir/${file%.c}.o"
		objects+=("$object_dir/${file%.c}.o")
	done

	"$cc" "${flags[@]}" "${defines[@]}" \
		-c "$root/bench/vm-bench.c" -o "$object_dir/vm-bench.o"
	objects+=("$object_dir/vm-bench.o")

	"$cc" "${objects[@]}" -lm -o "$out"
}

echo "building stock lua 5.5.1 (no patches, no hook) ..." >&2
build 0 0 "$work/bench-stock"
echo "building the shipped interpreter (back-edge checks, no hook) ..." >&2
build 1 0 "$work/bench-luaext"
echo "building the rejected design (count hook armed) ..." >&2
build 0 1 "$work/bench-hooked"

echo "running, best of $runs ..." >&2
"$work/bench-stock" "$runs" > "$work/stock.tsv"
"$work/bench-luaext" "$runs" > "$work/luaext.tsv"
"$work/bench-hooked" "$runs" > "$work/hooked.tsv"

printf '\n%-22s %9s %9s %7s %9s %7s\n' \
	"benchmark" "stock" "shipped" "vs" "hooked" "vs"
printf '%-22s %9s %9s %7s %9s %7s\n' \
	"----------------------" "---------" "---------" "-------" "---------" "-------"

join -t$'\t' "$work/stock.tsv" "$work/luaext.tsv" |
	join -t$'\t' - "$work/hooked.tsv" |
	awk -F'\t' '
		{
			stock = $2 + 0; ours = $3 + 0; hooked = $4 + 0
			r_ours = (stock > 0) ? ours / stock : 0
			r_hook = (stock > 0) ? hooked / stock : 0
			printf "%-22s %8.4fs %8.4fs %6.2fx %8.4fs %6.2fx\n", $1, stock, ours, r_ours, hooked, r_hook
			t_stock += stock; t_ours += ours; t_hook += hooked
			if (r_ours > worst) { worst = r_ours; worst_name = $1 }
		}
		END {
			printf "%-22s %9s %9s %7s %9s %7s\n", \
				"----------------------", "---------", "---------", "-------", "---------", "-------"
			printf "%-22s %8.4fs %8.4fs %6.2fx %8.4fs %6.2fx\n", \
				"total", t_stock, t_ours, t_ours / t_stock, t_hook, t_hook / t_stock
			printf "\nshipped worst case: %s at %.2fx\n", worst_name, worst
		}
	'

cat <<'NOTE'

Three interpreters, one source tree, one compiler, one run:

  stock    third_party/lua-5.5.1 with LUAEXT_LUA_HOOKS=0 -- upstream, byte
           for byte, since every patch is guarded by that macro
  shipped  what luaext actually ships: the interrupt check compiled into the
           four back edges a loop can close through
  hooked   the design this replaced: a LUA_MASKCOUNT hook, whose body here does
           nothing at all

The gap between "shipped" and "hooked" is the whole reason for the lvm.c patch.
It is not the hook BODY -- that body is empty. Any non-zero hookmask sets
ci->u.l.trap, and a set trap makes vmfetch call luaG_traceexec once per
INSTRUCTION. Raising the hook count does not help, because the count throttles
the body and not the call.

None of this includes the PHP boundary, conversion, or the callback bridge.
This is the interpreter alone -- the only figure comparable to a Lua author's
own benchmark.
NOTE
