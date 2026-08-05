#!/usr/bin/env bash

set -Eeuo pipefail
IFS=$'\n\t'

usage() {
    cat <<'EOF'
Exercise a preloaded, persistent product FPM worker across repeated requests and
OPcache resets.

Usage: test-native-product-fpm.sh --candidate PHP --fpm PHP_FPM
EOF
}

candidate=
fpm=
while (($#)); do
    case $1 in
        --candidate)
            (($# >= 2)) || { usage >&2; exit 2; }
            candidate=$2
            shift 2
            ;;
        --fpm)
            (($# >= 2)) || { usage >&2; exit 2; }
            fpm=$2
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            printf 'unknown argument: %s\n' "$1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

[[ $(uname -s) == Linux && $(uname -m) == x86_64 ]] || {
    printf 'product FPM test requires native Linux x86_64\n' >&2
    exit 3
}
[[ -x $candidate ]] || {
    printf 'candidate PHP is not executable: %s\n' "$candidate" >&2
    exit 2
}
[[ -x $fpm ]] || {
    printf 'PHP-FPM is not executable: %s\n' "$fpm" >&2
    exit 2
}
command -v cgi-fcgi >/dev/null 2>&1 || {
    printf 'cgi-fcgi is required for the real FPM request\n' >&2
    exit 3
}

candidate_sapi_root=$(CDPATH='' cd -- "$(dirname -- "$(dirname -- "$candidate")")" && pwd -P)
fpm_sapi_root=$(CDPATH='' cd -- "$(dirname -- "$(dirname -- "$fpm")")" && pwd -P)
[[ $candidate_sapi_root == "$fpm_sapi_root" ]] || {
    printf 'candidate PHP and PHP-FPM are from different builds: %s / %s\n' \
        "$candidate_sapi_root" "$fpm_sapi_root" >&2
    exit 2
}

script_dir=$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
repo_root=$(CDPATH='' cd -- "$script_dir/../.." && pwd -P)
profile_root=$(CDPATH='' cd -- "$candidate_sapi_root/../.." && pwd -P)
product_manifest=$profile_root/build-manifest.json
executor_source=$repo_root/Zend/Native/Compiler/zend_native_executor.c
startup_source=$repo_root/Zend/zend.c

verify_fail_closed_product_architecture() {
    command -v python3 >/dev/null 2>&1 || {
        printf 'python3 is required to verify the native build manifest\n' >&2
        exit 3
    }
    [[ -f $product_manifest ]] || {
        printf 'native build manifest is missing: %s\n' "$product_manifest" >&2
        exit 1
    }
    python3 - "$product_manifest" "$candidate" <<'PY'
import json
from pathlib import Path
import sys

manifest_path = Path(sys.argv[1])
candidate = Path(sys.argv[2]).resolve()
manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
flags = manifest.get("configure", {}).get("flags", [])
binary = manifest.get("binary", {}).get("path")
if "--enable-native-engine" not in flags:
    raise SystemExit(f"native engine is not enabled in {manifest_path}")
if not isinstance(binary, str) or Path(binary).resolve() != candidate:
    raise SystemExit(
        f"manifest binary does not match candidate: {binary!r} / {candidate}"
    )
PY

    [[ -f $executor_source && -f $startup_source ]] || {
        printf 'native executor architecture sources are missing\n' >&2
        exit 1
    }
    grep -F -A1 'if (zend_native_executor_startup() == FAILURE) {' \
        "$startup_source" | grep -F 'return FAILURE;' >/dev/null || {
        printf 'native executor startup is not fail-closed\n' >&2
        exit 1
    }
    grep -F 'zend_execute_ex = zend_native_executor_execute_ex;' \
        "$executor_source" >/dev/null || {
        printf 'native executor is not installed as the global executor\n' >&2
        exit 1
    }

    local executor_body
    executor_body=$(awk '
        /^void zend_native_executor_execute_ex\(zend_execute_data \*execute_data\)$/ {
            capture = 1
        }
        capture { print }
        capture && /^}$/ { exit }
    ' "$executor_source")
    [[ -n $executor_body ]] || {
        printf 'native product executor implementation was not found\n' >&2
        exit 1
    }
    grep -F 'zend_native_compiler_execute_observed_data(' \
        <<<"$executor_body" >/dev/null || {
        printf 'native product executor does not enter compiled execution\n' >&2
        exit 1
    }
    grep -F 'Native userland execution failed' \
        <<<"$executor_body" >/dev/null || {
        printf 'native product executor does not fail closed on execution errors\n' >&2
        exit 1
    }
    if grep -F 'zend_execute_ex(' <<<"$executor_body" >/dev/null; then
        printf 'native product executor contains a VM executor fallback\n' >&2
        exit 1
    fi
}

verify_fail_closed_product_architecture

work=$(mktemp -d "${TMPDIR:-/tmp}/php-native-product-fpm.XXXXXXXX")
fpm_pid=
cleanup() {
    if [[ -n $fpm_pid ]] && kill -0 "$fpm_pid" 2>/dev/null; then
        kill "$fpm_pid" 2>/dev/null || true
        wait "$fpm_pid" 2>/dev/null || true
    fi
    rm -rf -- "$work"
}
trap cleanup EXIT

socket=$work/native-product-fpm.sock
config=$work/php-fpm.conf
preload=$work/preload.php
included=$work/included.php
request=$work/request.php
responses=$work/responses
mkdir "$responses"

cat >"$config" <<EOF
[global]
daemonize = no
error_log = $work/fpm-error.log

[native-product]
listen = $socket
pm = static
pm.max_children = 1
pm.max_requests = 0
catch_workers_output = yes
clear_env = no
EOF

cat >"$preload" <<'PHP'
<?php
function native_product_preloaded_leaf(int $value): int
{
    return $value + 1;
}
PHP

cat >"$included" <<'PHP'
<?php
return static fn (int $value): int => $value + 2;
PHP

cat >"$request" <<'PHP'
<?php
function native_product_fpm_sum(int $count): int
{
    $value = 0;
    for ($index = 0; $index < $count; $index++) {
        $value = native_product_preloaded_leaf($value);
    }
    return $value;
}

function native_product_fpm_generator(): Generator
{
    yield native_product_fpm_sum(3);
    yield native_product_fpm_sum(5);
}

$generator = native_product_fpm_generator();
$first = $generator->current();
$generator->next();
$second = $generator->current();
$fiber = new Fiber(static function (): int {
    $value = Fiber::suspend(native_product_fpm_sum(7));
    return $value + 1;
});
$suspended = $fiber->start();
$includedPath = __DIR__ . '/included.php';
$included = include $includedPath;
$includedCachedBeforeReset = opcache_is_script_cached($includedPath);
$evaluated = eval('return native_product_fpm_sum(11);');
$reset = ($_GET['reset'] ?? '') === 'yes';
if ($reset) {
    printf("reset=%s\n", opcache_reset() ? 'yes' : 'no');
}
$fiber->resume(10);
printf(
    "phase=%s pid=%d sum=%d generator=%d,%d fiber=%d,%d "
        . "include=%d include_cached_before_reset=%s eval=%d\n",
    $_GET['phase'] ?? 'missing',
    getmypid(),
    native_product_fpm_sum(13),
    $first,
    $second,
    $suspended,
    $fiber->getReturn(),
    $included(40),
    $includedCachedBeforeReset ? 'yes' : 'no',
    $evaluated,
);
$opcache = function_exists('opcache_get_status')
    ? opcache_get_status(false)
    : false;
$statistics = is_array($opcache)
    ? ($opcache['opcache_statistics'] ?? [])
    : [];
printf(
    "opcache=%s hits=%d misses=%d cached=%d\n",
    is_array($opcache) ? 'on' : 'off',
    (int) ($statistics['hits'] ?? 0),
    (int) ($statistics['misses'] ?? 0),
    (int) ($statistics['num_cached_scripts'] ?? 0),
);
PHP

"$fpm" -n -y "$config" -F -O \
    -d opcache.enable=1 \
    -d opcache.validate_timestamps=0 \
    -d opcache.file_update_protection=0 \
    -d "opcache.preload=$preload" \
    >"$work/fpm.log" 2>&1 &
fpm_pid=$!

deadline=$((SECONDS + 15))
while [[ ! -S $socket ]]; do
    if ! kill -0 "$fpm_pid" 2>/dev/null; then
        cat "$work/fpm.log" >&2
        printf 'php-fpm exited before creating its socket\n' >&2
        exit 1
    fi
    if ((SECONDS >= deadline)); then
        cat "$work/fpm.log" >&2
        printf 'php-fpm did not create its socket\n' >&2
        exit 1
    fi
    sleep 0.01
done

request_fpm() {
    local phase=$1
    local reset=$2
    local output=$3
    SCRIPT_FILENAME=$request \
    SCRIPT_NAME=/request.php \
    REQUEST_METHOD=GET \
    REQUEST_URI="/request.php?phase=$phase&reset=$reset" \
    QUERY_STRING="phase=$phase&reset=$reset" \
    SERVER_PROTOCOL=HTTP/1.1 \
    GATEWAY_INTERFACE=CGI/1.1 \
    SERVER_SOFTWARE=native-product-test \
    REMOTE_ADDR=127.0.0.1 \
    SERVER_ADDR=127.0.0.1 \
    SERVER_PORT=9000 \
    CONTENT_LENGTH=0 \
        cgi-fcgi -bind -connect "$socket" >"$output"
}

worker_pid=
maximum_hits=0
reset_count=0
request_count=12
for ((index = 1; index <= request_count; index++)); do
    phase=cycle-$index
    reset=no
    if ((index == 4 || index == 8)); then
        reset=yes
        ((reset_count += 1))
    fi
    response=$responses/response-$index.txt
    request_fpm "$phase" "$reset" "$response"

    grep -F "phase=$phase" "$response" >/dev/null
    expected_include=42
    if ((index >= 5 && index <= 8)); then
        expected_include=43
    elif ((index >= 9)); then
        expected_include=44
    fi
    grep -F \
        "sum=13 generator=3,5 fiber=7,11 include=$expected_include include_cached_before_reset=yes eval=11" \
        "$response" >/dev/null
    grep -F 'opcache=on' "$response" >/dev/null
    if [[ $reset == yes ]]; then
        grep -F 'reset=yes' "$response" >/dev/null
    fi

    current_pid=$(sed -n \
        "s/.*phase=$phase pid=\([0-9][0-9]*\).*/\1/p" "$response")
    [[ -n $current_pid ]] || {
        printf 'request %d did not report a worker PID\n' "$index" >&2
        exit 1
    }
    [[ $current_pid != "$fpm_pid" ]] || {
        printf 'request %d executed in the FPM master process %s\n' \
            "$index" "$fpm_pid" >&2
        exit 1
    }
    if [[ -z $worker_pid ]]; then
        worker_pid=$current_pid
    elif [[ $current_pid != "$worker_pid" ]]; then
        printf 'request %d changed persistent worker: %s / %s\n' \
            "$index" "$worker_pid" "$current_pid" >&2
        exit 1
    fi

    hits=$(sed -n 's/.*opcache=on hits=\([0-9][0-9]*\).*/\1/p' \
        "$response")
    cached=$(sed -n 's/.* cached=\([0-9][0-9]*\).*/\1/p' "$response")
    [[ -n $hits && -n $cached && $cached -ge 1 ]] || {
        printf 'request %d has invalid OPcache statistics: hits=%s cached=%s\n' \
            "$index" "$hits" "$cached" >&2
        exit 1
    }
    if ((hits > maximum_hits)); then
        maximum_hits=$hits
    fi

    if ((index == 4)); then
        cat >"$included" <<'PHP'
<?php
return static fn (int $value): int => $value + 3;
PHP
    elif ((index == 8)); then
        cat >"$included" <<'PHP'
<?php
return static fn (int $value): int => $value + 4;
PHP
    fi
done

((maximum_hits > 0)) || {
    printf 'persistent FPM requests did not produce an OPcache hit\n' >&2
    exit 1
}

printf '%s\n' \
    "PASS product_fpm=1 preload=1 forked_worker=1 same_worker=1 requests=$request_count resets=$reset_count opcache_hits=1 include_generations=3 include_eval=1 suspended_fiber_reset=1 native_build_manifest=verified native_execution_evidence=fail_closed_product_executor_and_successful_fpm_userland"
