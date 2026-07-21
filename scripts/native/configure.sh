#!/usr/bin/env bash

set -Eeuo pipefail
IFS=$'\n\t'

SCRIPT_DIR="$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
# shellcheck source=lib/common.sh
source "$SCRIPT_DIR/lib/common.sh"
trap native_release_lock EXIT

usage() {
    cat <<'EOF'
Configure an isolated php-src W00 build profile.

Usage: configure.sh [--profile PROFILE] [--force] [--print-build-dir]

Options:
  --profile PROFILE    Build profile (default: debug-nts).
  --force              Re-run configure even when the state is compatible.
  --print-build-dir    Print the out-of-tree build directory after configuring.
  -h, --help           Show this help.

Environment:
  NATIVE_WORK_ROOT     Absolute external artifact root.
  CC, CFLAGS, CPPFLAGS, LDFLAGS
                       Toolchain overrides included in the state fingerprint.
EOF
}

profile=debug-nts
force=0
print_build_dir=0
while (($#)); do
    case $1 in
        --profile)
            (($# >= 2)) || native_die "--profile requires a value"
            profile=$2
            shift 2
            ;;
        --force)
            force=1
            shift
            ;;
        --print-build-dir)
            print_build_dir=1
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

for tool in git python3 autoconf autoheader bison re2c make pkg-config; do
    native_require_tool "$tool"
done
compiler=${CC:-cc}
native_require_tool "$compiler"

native_load_profile "$profile"
native_prepare_profile_paths "$profile"
native_configure_args
source_fingerprint=$(native_source_fingerprint)

native_acquire_lock "$NATIVE_WORKTREE_ROOT/.buildconf.lock"
buildconf_stamp="$NATIVE_WORKTREE_ROOT/buildconf-source.sha256"
recorded_buildconf_fingerprint=
if [[ -f $buildconf_stamp ]]; then
    recorded_buildconf_fingerprint=$(<"$buildconf_stamp")
fi
if [[ ! -x $NATIVE_REPO_ROOT/configure || $recorded_buildconf_fingerprint != "$source_fingerprint" ]]; then
    buildconf_log="$NATIVE_WORKTREE_ROOT/buildconf.log"
    printf 'Generating configure from %s\n' "$NATIVE_REPO_ROOT/configure.ac"
    set +e
    (cd "$NATIVE_REPO_ROOT" && ./buildconf --force) 2>&1 | tee "$buildconf_log"
    buildconf_status=${PIPESTATUS[0]}
    set -e
    ((buildconf_status == 0)) || native_die "buildconf failed; see $buildconf_log"
    printf '%s\n' "$source_fingerprint" >"$buildconf_stamp"
fi
native_release_lock

fingerprint=$(native_configuration_fingerprint "$compiler")
if ((force == 0)) && [[ -f $NATIVE_BUILD_DIR/Makefile ]] && \
    "$NATIVE_HELPER" state-matches --path "$NATIVE_STATE_PATH" --fingerprint "$fingerprint"; then
    printf 'Configuration is compatible; reusing %s\n' "$NATIVE_BUILD_DIR"
else
    native_acquire_lock "$NATIVE_PROFILE_ROOT/.configure.lock"
    if ((force == 0)) && [[ -f $NATIVE_BUILD_DIR/Makefile ]] && \
        "$NATIVE_HELPER" state-matches --path "$NATIVE_STATE_PATH" --fingerprint "$fingerprint"; then
        printf 'Configuration became available while waiting; reusing %s\n' "$NATIVE_BUILD_DIR"
    else
        native_reset_build_dir "$NATIVE_BUILD_DIR"
        configure_log="$NATIVE_LOG_DIR/configure.log"
        printf 'Configuring profile %s in %s\n' "$profile" "$NATIVE_BUILD_DIR"
        native_print_command "$NATIVE_REPO_ROOT/configure" "${NATIVE_CONFIGURE_ARGS[@]}" | tee "$NATIVE_LOG_DIR/configure-command.log"
        set +e
        (
            cd "$NATIVE_BUILD_DIR"
            CC="$compiler" "$NATIVE_REPO_ROOT/configure" "${NATIVE_CONFIGURE_ARGS[@]}"
        ) 2>&1 | tee "$configure_log"
        configure_status=${PIPESTATUS[0]}
        set -e
        ((configure_status == 0)) || native_die "configure failed for $profile; see $configure_log"
        "$NATIVE_HELPER" write-state \
            --path "$NATIVE_STATE_PATH" \
            --fingerprint "$fingerprint" \
            --profile "$profile" \
            --commit "$(native_commit)" \
            --source-fingerprint "$source_fingerprint"
    fi
    native_release_lock
fi

if ((print_build_dir)); then
    printf '%s\n' "$NATIVE_BUILD_DIR"
else
    printf 'BUILD_DIR=%s\n' "$NATIVE_BUILD_DIR"
fi
