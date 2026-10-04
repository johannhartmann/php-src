#!/usr/bin/env bash

set -Eeuo pipefail
IFS=$'\n\t'

SCRIPT_DIR="$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
# shellcheck source=lib/common.sh
source "$SCRIPT_DIR/lib/common.sh"

usage() {
    cat <<'EOF'
Run native PHPT suites in tiers, building profiles incrementally.

Usage: test-phpt.sh [--tier quick|commit|full] [--jobs N] [--no-build]
                    [--show-slow MS] [--baseline FILE] [PATH...]

Tiers:
  quick   (default) debug profile: ext/native_mir_test/tests plus PATHs.
          Seconds; run it after every change.
  commit  debug profile: the native, Zend, OPcache, array, math, string,
          SPL and reflection suites plus PATHs. Run it before a commit.
  full    commit, then ASan and UBSan, built concurrently and run one after
          the other. Run it before a push.

Options:
  --jobs N         Worker count (default: NATIVE_JOBS or CPU count).
  --no-build       Test the existing profile binaries without building.
  --show-slow MS   Report tests slower than MS milliseconds (default 3000).
  --baseline FILE  Also list failures that are not in FILE, one test path
                   per line (as written to the failures file of a run).

Environment:
  NATIVE_PHPT_RUNNER   Plain PHP CLI that runs run-tests.php (required; the
                       native CLI cannot run it yet).

Every profile run writes phpt-<tier>.log, phpt-<tier>-failures.txt and
phpt-<tier>-slow.txt below its NATIVE_WORK_ROOT log directory. Exit status:
0 when every run passed, 1 on test failures, 2 on usage errors, 3 on missing
prerequisites.
EOF
}

tier=quick
jobs=$(native_default_jobs)
build=1
show_slow=3000
baseline=
extra_paths=()
while (($#)); do
    case $1 in
        --tier)
            (($# >= 2)) || native_die "--tier requires a value"
            tier=$2
            shift 2
            ;;
        --jobs)
            (($# >= 2)) || native_die "--jobs requires a value"
            jobs=$2
            shift 2
            ;;
        --no-build)
            build=0
            shift
            ;;
        --show-slow)
            (($# >= 2)) || native_die "--show-slow requires a value"
            show_slow=$2
            shift 2
            ;;
        --baseline)
            (($# >= 2)) || native_die "--baseline requires a value"
            baseline=$2
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        -*)
            native_error "unknown argument: $1"
            exit 2
            ;;
        *)
            extra_paths+=("$1")
            shift
            ;;
    esac
done
native_validate_jobs "$jobs"
[[ $show_slow =~ ^[0-9]+$ ]] || { native_error "--show-slow must be a number"; exit 2; }
case $tier in
    quick|commit|full) ;;
    *) native_error "unknown tier: $tier"; exit 2 ;;
esac
if [[ -z ${NATIVE_PHPT_RUNNER:-} || ! -x ${NATIVE_PHPT_RUNNER:-} ]]; then
    native_error "NATIVE_PHPT_RUNNER must name a plain PHP CLI"
    exit 3
fi
if [[ -n $baseline && ! -f $baseline ]]; then
    native_error "baseline file not found: $baseline"
    exit 2
fi

case $(uname -s)-$(uname -m) in
    Linux-x86_64) prefix=linux-amd64-native ;;
    Darwin-arm64) prefix=darwin-arm64-native ;;
    *) native_error "no native profiles for this host"; exit 3 ;;
esac

commit_paths=(Zend/tests ext/native_mir_test/tests ext/opcache/tests
    ext/standard/tests/array ext/standard/tests/math
    ext/standard/tests/strings ext/spl/tests ext/reflection/tests)
sanitizer_paths=(Zend/tests ext/native_mir_test/tests ext/opcache/tests
    ext/standard/tests/array ext/standard/tests/math)
if [[ $tier == quick ]]; then
    debug_paths=(ext/native_mir_test/tests)
else
    debug_paths=("${commit_paths[@]}")
fi
debug_paths+=("${extra_paths[@]}")
sanitizer_paths+=("${extra_paths[@]}")

# Builds one profile with the given job count and prints its CLI binary.
build_profile() {
    local profile=$1 profile_jobs=$2
    if ((build)); then
        "$SCRIPT_DIR/build.sh" --profile "$profile" --jobs "$profile_jobs" \
            --print-binary | tail -n 1
    else
        (native_load_profile "$profile" && native_prepare_profile_paths "$profile" \
            && printf '%s\n' "$NATIVE_BINARY_PATH")
    fi
}

# Runs PHPT paths against one profile; returns run-tests' failure state.
run_profile() {
    local profile=$1 binary=$2 profile_jobs=$3
    shift 3
    local log failures slow args=()
    (
        native_load_profile "$profile"
        native_prepare_profile_paths "$profile"
        native_export_sanitizer_environment
        log="$NATIVE_LOG_DIR/phpt-$tier.log"
        failures="$NATIVE_LOG_DIR/phpt-$tier-failures.txt"
        slow="$NATIVE_LOG_DIR/phpt-$tier-slow.txt"
        args=(-n "$NATIVE_REPO_ROOT/run-tests.php" -q -j"$profile_jobs"
            --no-progress --set-timeout 300 --show-slow "$show_slow")
        [[ $PROFILE_SANITIZER == address ]] && args+=(--asan)
        cd "$NATIVE_REPO_ROOT"
        env -u PHP_INI_SCAN_DIR -u PHPRC \
            TEST_PHP_EXECUTABLE="$binary" TEST_PHP_SRCDIR="$NATIVE_REPO_ROOT" \
            "$NATIVE_PHPT_RUNNER" "${args[@]}" "$@" >"$log" 2>&1 || true
        sed 's/\x1b\[[0-9;]*m//g' "$log" \
            | sed -n 's/^\(FAIL\|LEAK\|BORK\) .*\[\([^]]*\)\].*/\2/p' \
            | LC_ALL=C sort -u >"$failures"
        sed 's/\x1b\[[0-9;]*m//g' "$log" \
            | sed -n '/^SLOW TEST SUMMARY/,/^=*$/p' >"$slow"
        printf '== %s: %s\n' "$profile" "$(sed 's/\x1b\[[0-9;]*m//g' "$log" \
            | grep -E '^(Tests failed|Tests passed|Time taken)' | tr -s ' ' \
            | paste -sd ';' -)"
        if [[ -s $failures ]]; then
            printf '   failures (%s):\n' "$failures"
            sed 's/^/     /' "$failures"
            if [[ -n $baseline ]]; then
                printf '   not in baseline:\n'
                LC_ALL=C comm -23 "$failures" <(LC_ALL=C sort -u "$baseline") \
                    | sed 's/^/     /'
            fi
        fi
        if [[ -s $slow ]]; then
            printf '   slow tests (> %s ms): %s\n' "$show_slow" "$slow"
        fi
        [[ ! -s $failures ]]
    )
}

status=0
debug_profile="$prefix-debug-nts"
debug_binary=$(build_profile "$debug_profile" "$jobs")
run_profile "$debug_profile" "$debug_binary" "$jobs" "${debug_paths[@]}" || status=1
# A host builds only its own backend; keep the other one compiling.
if [[ $prefix == linux-amd64-native ]]; then
    "$SCRIPT_DIR/check-darwin-backend.sh" --profile "$debug_profile" || status=1
fi

if [[ $tier == full ]]; then
    half=$(( jobs > 1 ? jobs / 2 : 1 ))
    asan_binary_file=$(mktemp)
    ubsan_binary_file=$(mktemp)
    build_profile "$prefix-asan-nts" "$half" >"$asan_binary_file" &
    asan_build=$!
    build_profile "$prefix-ubsan-nts" "$half" >"$ubsan_binary_file" &
    ubsan_build=$!
    wait "$asan_build" || native_die "ASan build failed"
    wait "$ubsan_build" || native_die "UBSan build failed"
    asan_binary=$(<"$asan_binary_file")
    ubsan_binary=$(<"$ubsan_binary_file")
    rm -f -- "$asan_binary_file" "$ubsan_binary_file"
    asan_status=0
    ubsan_status=0
    # Tests share the source tree (some write fixed files next to
    # themselves), so the two suites run one after the other.
    run_profile "$prefix-asan-nts" "$asan_binary" "$jobs" \
        "${sanitizer_paths[@]}" || asan_status=1
    run_profile "$prefix-ubsan-nts" "$ubsan_binary" "$jobs" \
        "${sanitizer_paths[@]}" || ubsan_status=1
    ((asan_status == 0 && ubsan_status == 0)) || status=1
fi
exit "$status"
