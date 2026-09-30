#!/usr/bin/env bash
# SPDX-License-Identifier: PHP-3.01
#
# Developer tool: regenerate the checked-in Linux x86-64 EncodeGen header
# from zend_tpde_encodegen.c and zend_tpde_encodegen_values.c. Normal builds
# never run this script; they use the checked-in header and need no LLVM.
#
# It builds tpde_encodegen from the TPDE revision recorded in
# ../ThirdParty/tpde/REVISION (TPDE_ENABLE_ENCODEGEN=ON, TPDE_ENABLE_LLVM=OFF)
# into $ENCODEGEN_ROOT (default $NATIVE_WORK_ROOT/tpde-encodegen) and compiles
# the snippets with the matching clang. zend_tpde_encodegen_values.c reads the
# Zend layouts of a configured release build, so no debug-only expansion
# (assertions, RC checks) reaches a snippet: ENCODEGEN_BUILD_DIR (default the
# linux-amd64-native-release-nts build under $NATIVE_WORK_ROOT). The tuning
# feature slow-incdec keeps refcount increments in instruction forms EncodeGen
# encodes; the ISA stays -march=x86-64. -fno-jump-tables keeps switches free
# of indirect branches and -fno-builtin keeps loops from becoming library
# calls, neither of which EncodeGen encodes. A snippet EncodeGen
# cannot encode fails the regeneration. The last regeneration used LLVM and
# clang 21.1.8 (TPDE's preferred version at the pin) from nixpkgs; set
# ENCODEGEN_TOOLCHAIN="" to use cmake, ninja, clang and LLVM from PATH.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../../.." && pwd)
root=${ENCODEGEN_ROOT:-${NATIVE_WORK_ROOT:-/tmp}/tpde-encodegen}
toolchain=${ENCODEGEN_TOOLCHAIN-nix shell nixpkgs#cmake nixpkgs#ninja nixpkgs#llvmPackages_21.llvm.dev nixpkgs#llvmPackages_21.llvm nixpkgs#llvmPackages_21.clang nixpkgs#lit --command}
revision=$(awk '$1 == "TPDE" { print $3 }' "$here/../ThirdParty/tpde/REVISION")
build=${ENCODEGEN_BUILD_DIR:-$(ls -d "${NATIVE_WORK_ROOT:-/nonexistent}"/php-src-*/linux-amd64-native-release-nts/build 2>/dev/null | head -1)}
if [ -z "$build" ] || [ ! -f "$build/main/php_config.h" ] \
		|| ! grep -q '^#define ZEND_DEBUG 0' "$build/main/php_config.h"; then
	echo "set ENCODEGEN_BUILD_DIR to a configured php-src release build" >&2
	exit 1
fi

mkdir -p "$root"
if [ ! -d "$root/tpde" ]; then
	git clone -q https://github.com/tpde2/tpde.git "$root/tpde"
fi
git -C "$root/tpde" fetch -q origin "$revision" 2>/dev/null || true
git -C "$root/tpde" checkout -q "$revision"
git -C "$root/tpde" submodule update -q --init --recursive

$toolchain bash -euo pipefail -c '
root=$1; here=$2; repo=$3; build=$4
llvm_dir=$(dirname "$(dirname "$(command -v llvm-config)")")
llvm_cmake=$(llvm-config --cmakedir 2>/dev/null || echo "$llvm_dir/lib/cmake/llvm")
if [ ! -x "$root/build/tpde-encodegen/tpde_encodegen" ]; then
	cmake -S "$root/tpde" -B "$root/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DTPDE_ENABLE_ENCODEGEN=ON -DTPDE_ENABLE_LLVM=OFF -DTPDE_INCLUDE_TESTS=OFF \
		-DLLVM_DIR="$llvm_cmake" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
		> "$root/configure.log"
	ninja -C "$root/build" tpde_encodegen > "$root/build.log"
fi
for source in zend_tpde_encodegen zend_tpde_encodegen_values; do
	clang -c -emit-llvm -ffreestanding -fcf-protection=none -O3 -fomit-frame-pointer \
		-fno-math-errno --target=x86_64-unknown-linux-gnu -march=x86-64 \
		-fno-jump-tables -fno-builtin -Xclang -target-feature -Xclang +slow-incdec \
		-I"$build" -I"$build/main" -I"$build/Zend" -I"$build/TSRM" \
		-I"$repo" -I"$repo/main" -I"$repo/Zend" -I"$repo/TSRM" \
		-o "$root/${source}_x64.bc" "$here/$source.c"
done
"$root/build/tpde-encodegen/tpde_encodegen" \
	-o "$root/zend_tpde_encodegen_x64.hpp" \
	"$root/zend_tpde_encodegen_x64.bc" "$root/zend_tpde_encodegen_values_x64.bc" \
	2> "$root/encodegen.log" || { cat "$root/encodegen.log" >&2; exit 1; }
if grep -q "Failed to generate" "$root/encodegen.log"; then
	grep -B2 "Failed to generate" "$root/encodegen.log" >&2
	exit 1
fi
' _ "$root" "$here" "$repo" "$build"

# The repository keeps generated headers free of trailing whitespace.
sed -e 's/[[:space:]]*$//' "$root/zend_tpde_encodegen_x64.hpp" \
	> "$here/../LinuxX64/zend_tpde_encodegen_x64.hpp"
echo "regenerated Zend/Native/TPDE/LinuxX64/zend_tpde_encodegen_x64.hpp"
