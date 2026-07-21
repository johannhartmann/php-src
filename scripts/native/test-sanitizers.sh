#!/usr/bin/env bash

set -Eeuo pipefail
IFS=$'\n\t'

SCRIPT_DIR="$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
# shellcheck source=lib/common.sh
source "$SCRIPT_DIR/lib/common.sh"

usage() {
    cat <<'EOF'
Build and smoke-test the W00 sanitizer profiles.

Usage: test-sanitizers.sh [--profile PROFILE] [--jobs N]

With no --profile, both mandatory sanitizer profiles are run. The harness uses
strict ASAN_OPTIONS/UBSAN_OPTIONS defaults, enables leak detection where the
platform runtime supports it, does not install suppressions, and fails on
diagnostics. An explicit profile may name any address- or
undefined-sanitizer profile.
EOF
}

selected_profile=
jobs=$(native_default_jobs)
while (($#)); do
    case $1 in
        --profile)
            (($# >= 2)) || native_die "--profile requires a value"
            selected_profile=$2
            shift 2
            ;;
        --jobs)
            (($# >= 2)) || native_die "--jobs requires a value"
            jobs=$2
            shift 2
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

if [[ -n $selected_profile ]]; then
    native_load_profile "$selected_profile"
    [[ $PROFILE_SANITIZER == address || $PROFILE_SANITIZER == undefined ]] || \
        native_die "profile is not a sanitizer profile: $selected_profile"
    profiles=("$selected_profile")
else
    profiles=(asan-nts ubsan-nts)
fi

status=0
for profile in "${profiles[@]}"; do
    printf 'Running sanitizer profile %s\n' "$profile"
    if ! "$SCRIPT_DIR/test-smoke.sh" --profile "$profile" --jobs "$jobs"; then
        native_error "sanitizer profile failed: $profile"
        status=1
    fi
done
exit "$status"
