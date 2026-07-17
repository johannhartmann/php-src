#!/usr/bin/env bash

set -Eeuo pipefail
IFS=$'\n\t'
umask 022

export LC_ALL=C
export LANG=C
export TZ=UTC

NATIVE_LIB_DIR="$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
NATIVE_REPO_ROOT="$(git -C "$NATIVE_LIB_DIR" rev-parse --show-toplevel 2>/dev/null)" || {
    printf 'native-w00: unable to locate the repository root\n' >&2
    exit 2
}
NATIVE_SCRIPTS_DIR="$NATIVE_REPO_ROOT/scripts/native"
NATIVE_PROFILES_DIR="$NATIVE_SCRIPTS_DIR/profiles"
NATIVE_HELPER="$NATIVE_SCRIPTS_DIR/capture-build-manifest.py"
NATIVE_W00_BASE_COMMIT=47355da494ba696b1bdb6d10448a225e742bd316
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-$(git -C "$NATIVE_REPO_ROOT" show -s --format=%ct HEAD)}

native_error() {
    printf 'native-w00: %s\n' "$*" >&2
}

native_die() {
    native_error "$*"
    exit 2
}

native_require_tool() {
    local tool=$1
    command -v "$tool" >/dev/null 2>&1 || native_die "required tool not found: $tool"
}

native_canonical_path() {
    local path=$1
    if [[ -d $path ]]; then
        (CDPATH='' cd -- "$path" && pwd -P)
    else
        local parent
        parent=$(CDPATH='' cd -- "$(dirname -- "$path")" && pwd -P)
        printf '%s/%s\n' "$parent" "$(basename -- "$path")"
    fi
}

native_commit() {
    git -C "$NATIVE_REPO_ROOT" rev-parse HEAD
}

native_base_commit() {
    if [[ -n ${NATIVE_BASE_COMMIT:-} ]]; then
        git -C "$NATIVE_REPO_ROOT" rev-parse --verify "${NATIVE_BASE_COMMIT}^{commit}"
    else
        printf '%s\n' "$NATIVE_W00_BASE_COMMIT"
    fi
}

native_worktree_id_for() {
    local worktree_path=$1
    local commit=$2
    local canonical base digest
    canonical=$(native_canonical_path "$worktree_path")
    base=$(printf '%s' "$(basename -- "$canonical")" | tr -c 'A-Za-z0-9._-' '-')
    digest=$(printf '%s\0%s' "$canonical" "$commit" | sha256sum | awk '{print substr($1, 1, 16)}')
    printf '%s-%s\n' "$base" "$digest"
}

native_worktree_id() {
    native_worktree_id_for "$NATIVE_REPO_ROOT" "$(native_base_commit)"
}

native_work_root() {
    local root=${NATIVE_WORK_ROOT:-${TMPDIR:-/tmp}/php-native-w00}
    if [[ $root != /* ]]; then
        native_die "NATIVE_WORK_ROOT must be an absolute path: $root"
    fi
    printf '%s\n' "${root%/}"
}

native_profile_names() {
    local file
    for file in "$NATIVE_PROFILES_DIR"/*.env; do
        basename -- "$file" .env
    done
}

native_profile_list() {
    native_profile_names | awk 'BEGIN { first = 1 } { if (!first) printf ", "; printf "%s", $0; first = 0 } END { print "" }'
}

native_load_profile() {
    local profile=$1
    local profile_file="$NATIVE_PROFILES_DIR/$profile.env"
    if [[ ! -f $profile_file ]]; then
        native_error "unknown profile '$profile'"
        native_error "available profiles: $(native_profile_list)"
        return 2
    fi

    unset PROFILE_NAME PROFILE_BUILD_TYPE PROFILE_THREAD_SAFETY PROFILE_SANITIZER
    unset PROFILE_CONFIGURE_FLAGS
    # Profile files are repository-owned declarative shell data.
    # shellcheck source=/dev/null
    source "$profile_file"

    [[ ${PROFILE_NAME:-} == "$profile" ]] || native_die "profile name mismatch in $profile_file"
    [[ ${PROFILE_BUILD_TYPE:-} == debug || ${PROFILE_BUILD_TYPE:-} == release ]] || native_die "invalid build type in $profile_file"
    [[ ${PROFILE_THREAD_SAFETY:-} == zts || ${PROFILE_THREAD_SAFETY:-} == nts ]] || native_die "invalid thread-safety value in $profile_file"
    [[ ${PROFILE_SANITIZER:-} == none || ${PROFILE_SANITIZER:-} == address || ${PROFILE_SANITIZER:-} == undefined ]] || native_die "invalid sanitizer value in $profile_file"
    declare -p PROFILE_CONFIGURE_FLAGS >/dev/null 2>&1 || native_die "missing configure flags in $profile_file"

    NATIVE_PROFILE=$profile
    NATIVE_PROFILE_FILE=$profile_file
}

native_prepare_profile_paths() {
    local profile=$1
    NATIVE_WORKTREE_ID=$(native_worktree_id)
    NATIVE_WORKTREE_ROOT="$(native_work_root)/$NATIVE_WORKTREE_ID"
    NATIVE_PROFILE_ROOT="$NATIVE_WORKTREE_ROOT/$profile"
    NATIVE_BUILD_DIR="$NATIVE_PROFILE_ROOT/build"
    NATIVE_LOG_DIR="$NATIVE_PROFILE_ROOT/logs"
    NATIVE_ARTIFACT_DIR="$NATIVE_PROFILE_ROOT/artifacts"
    NATIVE_STATE_PATH="$NATIVE_PROFILE_ROOT/configure-state.json"
    NATIVE_MANIFEST_PATH="$NATIVE_PROFILE_ROOT/build-manifest.json"
    NATIVE_BINARY_PATH="$NATIVE_BUILD_DIR/sapi/cli/php"
    mkdir -p -- "$NATIVE_BUILD_DIR" "$NATIVE_LOG_DIR" "$NATIVE_ARTIFACT_DIR"
}

native_configure_args() {
    NATIVE_CONFIGURE_ARGS=(
        --enable-option-checking=fatal
        --disable-all
        --enable-cli
        --disable-cgi
        --disable-phpdbg
        --without-pear
        "--prefix=$NATIVE_PROFILE_ROOT/install"
    )
    NATIVE_CONFIGURE_ARGS+=("${PROFILE_CONFIGURE_FLAGS[@]}")
}

native_default_jobs() {
    if [[ -n ${NATIVE_JOBS:-} ]]; then
        printf '%s\n' "$NATIVE_JOBS"
    elif command -v nproc >/dev/null 2>&1; then
        nproc
    else
        printf '2\n'
    fi
}

native_validate_jobs() {
    local jobs=$1
    [[ $jobs =~ ^[1-9][0-9]*$ ]] || native_die "jobs must be a positive integer: $jobs"
}

native_source_fingerprint() {
    {
        git -C "$NATIVE_REPO_ROOT" rev-parse HEAD
        git -C "$NATIVE_REPO_ROOT" status --porcelain=v1 --untracked-files=all
        git -C "$NATIVE_REPO_ROOT" diff --binary HEAD
        git -C "$NATIVE_REPO_ROOT" ls-files --others --exclude-standard | while IFS= read -r file; do
            printf '%s\0' "$file"
            sha256sum -- "$NATIVE_REPO_ROOT/$file"
        done
    } | sha256sum | awk '{print $1}'
}

native_configuration_fingerprint() {
    local cc=$1
    local args=()
    local arg
    for arg in "${NATIVE_CONFIGURE_ARGS[@]}"; do
        args+=("--configure-arg=$arg")
    done
    "$NATIVE_HELPER" fingerprint \
        --repo "$NATIVE_REPO_ROOT" \
        --profile-file "$NATIVE_PROFILE_FILE" \
        --compiler "$cc" \
        --env-cc "${CC:-}" \
        --env-cflags "${CFLAGS:-}" \
        --env-cppflags "${CPPFLAGS:-}" \
        --env-ldflags "${LDFLAGS:-}" \
        "${args[@]}"
}

native_reset_build_dir() {
    local build_dir=$1
    [[ -n $build_dir && $build_dir == "$(native_work_root)"/*/*/build ]] || native_die "refusing to reset unexpected build directory: $build_dir"
    mkdir -p -- "$build_dir"
    find "$build_dir" -mindepth 1 -depth -delete
}

native_acquire_lock() {
    local lock_path=$1
    mkdir -p -- "$(dirname -- "$lock_path")"
    exec {NATIVE_LOCK_FD}>"$lock_path"
    flock "$NATIVE_LOCK_FD"
}

native_print_command() {
    printf '%q ' "$@"
    printf '\n'
}

native_export_sanitizer_environment() {
    case ${PROFILE_SANITIZER:-none} in
        address)
            export ASAN_OPTIONS=${ASAN_OPTIONS:-abort_on_error=1:detect_leaks=1:halt_on_error=1:strict_string_checks=1}
            export LSAN_OPTIONS=${LSAN_OPTIONS:-exitcode=23:print_suppressions=1}
            export USE_ZEND_ALLOC=${USE_ZEND_ALLOC:-0}
            ;;
        undefined)
            export UBSAN_OPTIONS=${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}
            ;;
        none)
            ;;
    esac
}
