#!/usr/bin/env bash

set -Eeuo pipefail
IFS=$'\n\t'

SCRIPT_DIR="$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
# shellcheck source=lib/common.sh
source "$SCRIPT_DIR/lib/common.sh"

usage() {
    cat <<'EOF'
Run the focused W00 CLI, Zend, and OPcache smoke suite.

Usage: test-smoke.sh [--profile PROFILE | --php-binary PATH] [--jobs N]

Options:
  --profile PROFILE    Build/use an isolated profile (default: debug-nts).
  --php-binary PATH    Test an explicit PHP CLI binary instead of a profile.
  --jobs N             Jobs used when the selected profile must be built.
  -h, --help           Show this help.

Raw command/PHPT logs and smoke-summary.json are stored below NATIVE_WORK_ROOT.
Test failures and sanitizer diagnostics produce a non-zero exit status.
EOF
}

profile=debug-nts
profile_selected=0
explicit_binary=
jobs=$(native_default_jobs)
while (($#)); do
    case $1 in
        --profile)
            (($# >= 2)) || native_die "--profile requires a value"
            profile=$2
            profile_selected=1
            shift 2
            ;;
        --php-binary)
            (($# >= 2)) || native_die "--php-binary requires a value"
            explicit_binary=$2
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
if ((profile_selected)) && [[ -n $explicit_binary ]]; then
    native_die "--profile and --php-binary are mutually exclusive"
fi

if [[ -n $explicit_binary ]]; then
    binary=$(native_canonical_path "$explicit_binary")
    [[ -x $binary ]] || native_die "PHP binary is not executable: $binary"
    profile=external
    PROFILE_SANITIZER=none
    NATIVE_WORKTREE_ID=$(native_worktree_id)
    NATIVE_WORKTREE_ROOT="$(native_work_root)/$NATIVE_WORKTREE_ID"
    binary_id=$(printf '%s' "$binary" | sha256sum | awk '{print substr($1, 1, 16)}')
    smoke_dir="$NATIVE_WORKTREE_ROOT/external-$binary_id/artifacts/smoke"
else
    native_load_profile "$profile"
    native_prepare_profile_paths "$profile"
    native_export_sanitizer_environment
    "$SCRIPT_DIR/build.sh" --profile "$profile" --jobs "$jobs"
    binary=$NATIVE_BINARY_PATH
    smoke_dir="$NATIVE_ARTIFACT_DIR/smoke"
fi

mkdir -p -- "$smoke_dir"
version_log="$smoke_dir/php-version.log"
modules_log="$smoke_dir/php-modules.log"
phpt_log="$smoke_dir/phpt.log"
results_file="$smoke_dir/phpt-results.tsv"
summary_file="$smoke_dir/smoke-summary.json"
: >"$results_file"

set +e
"$binary" -n -v >"$version_log" 2>&1
version_status=$?
"$binary" -n -m >"$modules_log" 2>&1
modules_status=$?
set -e

if ((modules_status == 0)) && ! grep -Fx 'Zend OPcache' "$modules_log" >/dev/null; then
    printf 'native-w00: Zend OPcache is missing from php -m\n' >>"$modules_log"
    modules_status=1
fi

tests=(
    "$NATIVE_REPO_ROOT/Zend/tests/add_001.phpt"
    "$NATIVE_REPO_ROOT/sapi/cli/tests/001.phpt"
    "$NATIVE_REPO_ROOT/ext/opcache/tests/001_cli.phpt"
)
export NO_INTERACTION=1
export TEST_PHP_EXECUTABLE="$binary"
export TEST_PHP_SRCDIR="$NATIVE_REPO_ROOT"
set +e
"$binary" "$NATIVE_REPO_ROOT/run-tests.php" \
    -q -n -p "$binary" -W "$results_file" --offline --set-timeout 60 \
    "${tests[@]}" >"$phpt_log" 2>&1
phpt_status=$?
set -e

cat "$version_log"
cat "$modules_log"
cat "$phpt_log"

sanitizer_diagnostics=0
if grep -E \
    'ERROR: (AddressSanitizer|LeakSanitizer)|SUMMARY: (AddressSanitizer|UndefinedBehaviorSanitizer)|UndefinedBehaviorSanitizer|runtime error:' \
    "$version_log" "$modules_log" "$phpt_log" >/dev/null; then
    sanitizer_diagnostics=1
fi

summary_args=(
    write-summary
    --path "$summary_file"
    --profile "$profile"
    --binary "$binary"
    --sanitizer "${PROFILE_SANITIZER:-none}"
    --results "$results_file"
    --version-exit "$version_status"
    --modules-exit "$modules_status"
    --phpt-exit "$phpt_status"
)
if ((sanitizer_diagnostics)); then
    summary_args+=(--sanitizer-diagnostics)
fi
set +e
"$NATIVE_HELPER" "${summary_args[@]}"
summary_status=$?
set -e
printf 'SUMMARY=%s\n' "$summary_file"
if ((summary_status != 0)); then
    native_error "smoke test failed for $profile; see $summary_file"
    exit 1
fi
printf 'Smoke test passed for %s\n' "$profile"
