#!/usr/bin/env bash

set -Eeuo pipefail
IFS=$'\n\t'

SCRIPT_DIR="$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
# shellcheck source=lib/common.sh
source "$SCRIPT_DIR/lib/common.sh"

usage() {
    cat <<'EOF'
Build an isolated php-src W00 profile and write a JSON manifest.

Usage: build.sh [--profile PROFILE] [--jobs N] [--force-configure] [--print-binary]

Options:
  --profile PROFILE    Build profile (default: debug-nts).
  --jobs N             Parallel make jobs (default: NATIVE_JOBS or CPU count).
  --force-configure    Discard the profile build tree and reconfigure.
  --print-binary       Print the PHP CLI path as the final output line.
  -h, --help           Show this help.
EOF
}

profile=debug-nts
jobs=$(native_default_jobs)
force_configure=0
print_binary=0
while (($#)); do
    case $1 in
        --profile)
            (($# >= 2)) || native_die "--profile requires a value"
            profile=$2
            shift 2
            ;;
        --jobs)
            (($# >= 2)) || native_die "--jobs requires a value"
            jobs=$2
            shift 2
            ;;
        --force-configure)
            force_configure=1
            shift
            ;;
        --print-binary)
            print_binary=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            native_die "unknown argument: $1"
            ;;
    esac
done

native_validate_jobs "$jobs"
native_load_profile "$profile"
native_prepare_profile_paths "$profile"
native_configure_args
native_export_sanitizer_environment

configure_command=("$SCRIPT_DIR/configure.sh" --profile "$profile")
if ((force_configure)); then
    configure_command+=(--force)
fi
"${configure_command[@]}"

native_acquire_lock "$NATIVE_PROFILE_ROOT/.build.lock"
build_log="$NATIVE_LOG_DIR/build.log"
printf 'Building profile %s with %s job(s)\n' "$profile" "$jobs"
set +e
make -C "$NATIVE_BUILD_DIR" -j"$jobs" 2>&1 | tee "$build_log"
build_status=${PIPESTATUS[0]}
set -e
((build_status == 0)) || native_die "build failed for $profile; see $build_log"
[[ -x $NATIVE_BINARY_PATH ]] || native_die "build succeeded but PHP binary is missing: $NATIVE_BINARY_PATH"

runtime_state=$(
    "$NATIVE_BINARY_PATH" -n -r \
        'printf("%d %d %d", PHP_DEBUG ? 1 : 0, PHP_ZTS ? 1 : 0, extension_loaded("Zend OPcache") ? 1 : 0);'
)
IFS=' ' read -r runtime_debug runtime_zts opcache_loaded <<<"$runtime_state"
expected_debug=0
expected_zts=0
[[ $PROFILE_BUILD_TYPE == debug ]] && expected_debug=1
[[ $PROFILE_THREAD_SAFETY == zts ]] && expected_zts=1
[[ $runtime_debug == "$expected_debug" ]] || native_die "$profile produced unexpected PHP_DEBUG=$runtime_debug"
[[ $runtime_zts == "$expected_zts" ]] || native_die "$profile produced unexpected PHP_ZTS=$runtime_zts"
[[ $opcache_loaded == 1 ]] || native_die "$profile did not build OPcache into the CLI"

case $PROFILE_SANITIZER in
    address)
        grep -F -- '-fsanitize=address' "$NATIVE_BUILD_DIR/Makefile" >/dev/null || native_die "ASan flag missing from generated Makefile"
        ;;
    undefined)
        grep -F -- '-fsanitize=undefined' "$NATIVE_BUILD_DIR/Makefile" >/dev/null || native_die "UBSan flag missing from generated Makefile"
        ;;
esac

compiler=${CC:-cc}
fingerprint=$(native_configuration_fingerprint "$compiler")
manifest_args=()
for arg in "${NATIVE_CONFIGURE_ARGS[@]}"; do
    manifest_args+=("--configure-arg=$arg")
done
if ! "$NATIVE_HELPER" state-matches --path "$NATIVE_STATE_PATH" --fingerprint "$fingerprint"; then
    native_die "source or configuration changed during build; rerun build.sh"
fi
"$NATIVE_HELPER" write-manifest \
    --path "$NATIVE_MANIFEST_PATH" \
    --repo "$NATIVE_REPO_ROOT" \
    --base-commit "$(native_base_commit)" \
    --worktree-id "$NATIVE_WORKTREE_ID" \
    --profile "$profile" \
    --build-type "$PROFILE_BUILD_TYPE" \
    --thread-safety "$PROFILE_THREAD_SAFETY" \
    --sanitizer "$PROFILE_SANITIZER" \
    --configure-program "$NATIVE_REPO_ROOT/configure" \
    "${manifest_args[@]}" \
    --fingerprint "$fingerprint" \
    --compiler "$compiler" \
    --build-dir "$NATIVE_BUILD_DIR" \
    --binary "$NATIVE_BINARY_PATH" \
    --runtime-debug "$runtime_debug" \
    --runtime-zts "$runtime_zts" \
    --opcache-loaded "$opcache_loaded" \
    --jobs "$jobs"

flock -u "$NATIVE_LOCK_FD"
printf 'MANIFEST=%s\n' "$NATIVE_MANIFEST_PATH"
if ((print_binary)); then
    printf '%s\n' "$NATIVE_BINARY_PATH"
else
    printf 'BINARY=%s\n' "$NATIVE_BINARY_PATH"
fi
