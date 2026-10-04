#!/usr/bin/env bash
# Syntax-checks the Darwin arm64 backend on a Linux host.
#
# A host builds only its own target backend (ADR 0006), so a Linux build does
# not compile Zend/Native/TPDE/DarwinA64. This check compiles the Darwin
# sources with -fsyntax-only and the exact flags of the configured profile, so
# shared-code changes that break the Darwin backend fail early. Nothing is
# linked. Sections guarded by __APPLE__ are only checked by the Darwin build.

set -Eeuo pipefail
IFS=$'\n\t'

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=lib/common.sh
source "$SCRIPT_DIR/lib/common.sh"

usage() {
    cat <<'EOF'
Syntax-check the Darwin arm64 backend against the shared native headers.

Usage: check-darwin-backend.sh [--profile PROFILE]

The profile (default linux-amd64-native-debug-nts) must be configured; its
compile flags are reused. Exit status: 0 when every source parses, 1 on a
compile error, 2 on usage errors, 3 when the profile is not configured.
EOF
}

profile=linux-amd64-native-debug-nts
while (($#)); do
    case $1 in
        --profile)
            (($# >= 2)) || { usage >&2; exit 2; }
            profile=$2
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            native_error "unknown argument: $1"
            usage >&2
            exit 2
            ;;
    esac
done

native_load_profile "$profile"
native_prepare_profile_paths "$profile"
[[ -f $NATIVE_BUILD_DIR/Makefile ]] || {
    native_error "profile $profile is not configured: $NATIVE_BUILD_DIR"
    exit 3
}

# Reuse the compile command of a shared C++ backend source.
command_line=$(make -s -n -B -C "$NATIVE_BUILD_DIR" \
    Zend/Native/TPDE/Common/zend_tpde_backend.lo 2>/dev/null \
    | grep -F 'zend_tpde_backend.cpp' | grep -F -- '--mode=compile' | head -n 1)
[[ -n $command_line ]] || {
    native_error "cannot derive the C++ compile command from $NATIVE_BUILD_DIR"
    exit 3
}
command_line=${command_line#*--mode=compile }
command_line=${command_line%% -c *}
IFS=' ' read -r -a compile <<<"$command_line"

sources=(
    Zend/Native/TPDE/DarwinA64/zend_tpde_darwin_arm64.cpp
    Zend/Native/TPDE/DarwinA64/zend_tpde_darwin_assembler.cpp
    Zend/Native/Runtime/DarwinA64/zend_native_publish_darwin_arm64.cpp
)
status=0
pids=()
logs=()
for source in "${sources[@]}"; do
    log=$(mktemp)
    logs+=("$log")
    (cd "$NATIVE_BUILD_DIR" && "${compile[@]}" \
        -I"$NATIVE_REPO_ROOT/Zend/Native/TPDE/ThirdParty/tpde/disarm" \
        -fsyntax-only "$NATIVE_REPO_ROOT/$source") >"$log" 2>&1 &
    pids+=($!)
done
for index in "${!pids[@]}"; do
    if ! wait "${pids[$index]}"; then
        status=1
        native_error "Darwin backend does not compile: ${sources[$index]}"
        grep -E 'error:' "${logs[$index]}" | head -n 20 >&2 || true
    fi
    rm -f -- "${logs[$index]}"
done
((status == 0)) && printf '== darwin backend: syntax check passed (%d sources)\n' "${#sources[@]}"
exit "$status"
