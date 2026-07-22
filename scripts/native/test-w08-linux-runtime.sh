#!/usr/bin/env bash

set -Eeuo pipefail
IFS=$'\n\t'

usage() {
    cat <<'EOF'
Exercise the W08 runtime through a real Linux FPM request and a separate
native timeout process.

Usage: test-w08-linux-runtime.sh --candidate PHP --fpm PHP_FPM
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
    printf 'W08 Linux runtime test requires native Linux x86_64\n' >&2
    exit 2
}
[[ -x $candidate ]] || { printf 'candidate PHP is not executable: %s\n' "$candidate" >&2; exit 2; }
[[ -x $fpm ]] || { printf 'PHP-FPM is not executable: %s\n' "$fpm" >&2; exit 2; }
command -v cgi-fcgi >/dev/null 2>&1 || {
    printf 'cgi-fcgi is required for the real FPM request\n' >&2
    exit 2
}
command -v timeout >/dev/null 2>&1 || {
    printf 'GNU timeout is required for the isolated timeout test\n' >&2
    exit 2
}

work=$(mktemp -d "${TMPDIR:-/tmp}/php-native-w08.XXXXXXXX")
fpm_pid=
cleanup() {
    if [[ -n $fpm_pid ]] && kill -0 "$fpm_pid" 2>/dev/null; then
        kill "$fpm_pid" 2>/dev/null || true
        wait "$fpm_pid" 2>/dev/null || true
    fi
    rm -rf -- "$work"
}
trap cleanup EXIT

socket=$work/native-fpm.sock
fpm_config=$work/php-fpm.conf
php_ini=$work/php.ini
request=$work/request.php
response=$work/response.txt

cat >"$fpm_config" <<EOF
[global]
daemonize = no
error_log = $work/fpm-error.log

[www]
listen = $socket
pm = static
pm.max_children = 1
catch_workers_output = yes
clear_env = no
EOF

cat >"$php_ini" <<'EOF'
display_errors=1
log_errors=0
zend_test.observer.enabled=1
zend_test.observer.show_output=1
zend_test.observer.observe_function_names=native_fpm_outer,native_fpm_inner,intdiv,strcmp
zend_test.observer.show_return_value=0
zend_test.observer.execute_internal=1
EOF

cat >"$request" <<'PHP'
<?php
$source = <<<'NATIVE_PHP'
<?php
function native_fpm_inner(): int
{
    try {
        intdiv(1, 0);
    } catch (DivisionByZeroError) {
        strcmp('same', 'same');
        return 42;
    }
    return 0;
}

function native_fpm_outer(): int
{
    return native_fpm_inner();
}
NATIVE_PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w08-fpm-native.php',
    [],
    ['wave' => 8, 'function' => 'native_fpm_outer'],
);
printf(
    "accepted=%s execution=%s return=%s exception=%d bailout=%d vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    $result['execution']['status'],
    json_encode($result['execution']['return_value']),
    $result['execution']['exception'],
    $result['execution']['bailout'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
);
PHP

"$fpm" -F -y "$fpm_config" -c "$php_ini" >"$work/fpm.log" 2>&1 &
fpm_pid=$!
for _ in {1..100}; do
    [[ -S $socket ]] && break
    kill -0 "$fpm_pid" 2>/dev/null || {
        cat "$work/fpm.log" >&2
        exit 1
    }
    sleep 0.1
done
[[ -S $socket ]] || { printf 'PHP-FPM socket was not created\n' >&2; exit 1; }

SCRIPT_FILENAME=$request \
SCRIPT_NAME=/request.php \
REQUEST_URI=/request.php \
REQUEST_METHOD=GET \
SERVER_PROTOCOL=HTTP/1.1 \
GATEWAY_INTERFACE=CGI/1.1 \
SERVER_SOFTWARE=native-w08-test \
REMOTE_ADDR=127.0.0.1 \
REMOTE_PORT=12345 \
SERVER_ADDR=127.0.0.1 \
SERVER_PORT=9000 \
    cgi-fcgi -bind -connect "$socket" >"$response"

grep -F '<native_fpm_outer>' "$response" >/dev/null
grep -F '<native_fpm_inner>' "$response" >/dev/null
grep -F '<intdiv>' "$response" >/dev/null
grep -F '<strcmp>' "$response" >/dev/null
grep -F \
    'accepted=accepted execution=returned return=42 exception=0 bailout=0 vm=0 execute_ex=0 handler=0' \
    "$response" >/dev/null

timeout_source=$work/timeout.php
timeout_output=$work/timeout-output.txt
cat >"$timeout_source" <<'PHP'
<?php
$source = <<<'NATIVE_PHP'
<?php
function native_linux_timeout(): int
{
    while (true) {
    }
    return 0;
}
NATIVE_PHP;

set_time_limit(1);
$result = native_mir_test_compile_execute(
    $source,
    'w08-linux-timeout.php',
    [],
    ['wave' => 8, 'function' => 'native_linux_timeout'],
);
printf(
    "status=%s execution=%s exception=%d bailout=%d vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    $result['execution']['status'],
    $result['execution']['exception'],
    $result['execution']['bailout'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
);
PHP

set +e
timeout 10s "$candidate" -n -d display_errors=1 -d log_errors=0 \
    "$timeout_source" >"$timeout_output" 2>&1
timeout_status=$?
set -e
[[ $timeout_status == 0 || $timeout_status == 255 ]] || {
    cat "$timeout_output" >&2
    printf 'timeout process exited %d\n' "$timeout_status" >&2
    exit 1
}
grep -F 'Maximum execution time of 1 second exceeded' "$timeout_output" >/dev/null
grep -F \
    'status=error execution=bailout exception=0 bailout=1 vm=0 execute_ex=0 handler=0' \
    "$timeout_output" >/dev/null

printf 'PASS real_fpm=1 internal_calls=2 exception_caught=1 observer=1 timeout_process=1 vm_dispatch=0\n'
