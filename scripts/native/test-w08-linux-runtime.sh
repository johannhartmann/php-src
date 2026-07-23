#!/usr/bin/env bash

set -Eeuo pipefail
IFS=$'\n\t'

usage() {
    cat <<'EOF'
Exercise the W08-W11 runtime through repeated real Linux FPM requests and a separate
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
response_one=$work/response-1.txt
response_two=$work/response-2.txt
response=$response_one

dump_failure() {
	local status=$?
	printf 'W08-W11 Linux runtime test failed at line %s: %s\n' \
		"${BASH_LINENO[0]}" "$BASH_COMMAND" >&2
	for artifact in "$response_one" "$response_two" \
			"$work/fpm.log" "$work/fpm-error.log"; do
		if [[ -s $artifact ]]; then
			printf '%s\n' "--- $artifact ---" >&2
			cat "$artifact" >&2
		fi
	done
	exit "$status"
}
trap dump_failure ERR

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
zend_test.observer.observe_function_names=native_fpm_outer,native_fpm_inner,w09_fpm_outer,w09_fpm_collect,w09_fpm_map,w09_fpm_ref,w10_fpm_outer,w10_fpm_loaded_callback,W10FpmObject::__construct,W10FpmObject::__get,W10FpmObject::__set,W10FpmObject::__destruct,W10FpmTrait::compute,w11_fpm_outer,w11_fpm_static,W11FpmStream::stream_open,W11FpmStream::stream_read,W11FpmStream::stream_eof,W11FpmStream::stream_stat,W11FpmStream::url_stat,W11FpmRuntime::value,W11FpmChild::value,W11FpmBase::base,intdiv,strcmp,array_map
zend_test.observer.show_return_value=0
zend_test.observer.execute_internal=1
EOF

mkdir "$work/w11-library"
cat >"$work/w11-library/path-unit.php" <<'PHP'
<?php
return 20;
PHP
cat >"$work/w11-library/base.php" <<'PHP'
<?php
class W11FpmBase {
    public function base(): int {
        return 40;
    }
}
PHP
cat >"$work/w11-library/child.php" <<'PHP'
<?php
class W11FpmChild extends W11FpmBase {
    public function value(): int {
        return $this->base() + 2;
    }
}
PHP

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

$w09Source = <<<'NATIVE_PHP'
<?php
function &w09_fpm_ref(&$value)
{
    return $value;
}

function w09_fpm_collect(&$values, $label = 'default', ...$extra)
{
    $values[] = $label;
    foreach ($extra as $key => $value) {
        $values[$key] = $value;
    }
    return $values;
}

function w09_fpm_map($value)
{
    return "mapped-$value";
}

function w09_fpm_outer()
{
    $input = ['numbers' => [1, 2, 3], 'stable' => 'source'];
    $original = $input;
    $numbers =& $input['numbers'];
    foreach ($numbers as &$number) {
        $number *= 2;
    }
    unset($number);
    $returned =& w09_fpm_ref($numbers);
    $returned[] = 7;
    $arguments = ['label' => 'named', 'tail' => 'unpacked'];
    $called = w09_fpm_collect($numbers, ...$arguments);
    $mapped = array_map('w09_fpm_map', ['a', 'b']);
    $events = [];
    try {
        intdiv(1, 0);
    } catch (DivisionByZeroError) {
        $events[] = 'catch';
    } finally {
        $events[] = 'finally';
    }
    $rope = "A{$numbers[0]}B{$numbers[1]}C{$numbers[2]}D";
    $selected = $input;
    for ($index = 0; $index < 3; $index++) {
        $selected['loop'][] = $index;
    }
    return [$original, $input, $called, $mapped, $events, $rope, $selected['loop']];
}
NATIVE_PHP;

$w09 = native_mir_test_compile_execute(
    $w09Source,
    'w09-fpm-native.php',
    [],
    ['wave' => 9, 'function' => 'w09_fpm_outer', 'repeat' => 10],
);
printf(
    "w09=%s execution=%s return=%s runs=%d vm=%d execute_ex=%d handler=%d\n",
    $w09['status'],
    $w09['execution']['status'],
    json_encode($w09['execution']['return_value']),
    $w09['execution']['executions'],
    $w09['execution']['vm_handler_calls'],
    $w09['execution']['execute_ex_calls'],
    $w09['execution']['opline_handler_calls'],
);

class W10FpmLoadedBase {
    protected int $base = 30;
    public function base(): int { return $this->base; }
}
function w10_fpm_loaded_callback(int $value): int { return $value + 1; }

$w10Source = <<<'NATIVE_PHP'
<?php
interface W10FpmContract { public function compute(int $value): int; }
trait W10FpmTrait {
    public function compute(int $value): int { return $this->base() + $value; }
}
class W10FpmObject extends W10FpmLoadedBase implements W10FpmContract {
    use W10FpmTrait;
    private array $values = [];
    public static int $destructed = 0;
    public function __construct(public readonly int $offset) {}
    public function __get(string $name): mixed { return $this->values[$name] ?? null; }
    public function __set(string $name, mixed $value): void { $this->values[$name] = $value; }
    public function __destruct() {
        self::$destructed = w10_fpm_loaded_callback(self::$destructed);
    }
}
function w10_fpm_outer(): int {
    W10FpmObject::$destructed = 39;
    $class = 'W10FpmObject';
    $object = new $class(10);
    $object->answer = 40;
    $captured = 1;
    $callback = function(int $value) use (&$captured, $object): int {
        $captured++;
        $method = 'compute';
        return $object->$method($value) + $captured;
    };
    $mapped = array_map($callback, [8]);
    try {
        throw new RuntimeException('fpm');
    } catch (RuntimeException $error) {
        $value = $error->getMessage() === 'fpm'
            ? $mapped[0] + $object->answer - 40
            : 0;
    } finally {
        $value += 2;
    }
    unset($object, $callback);
    gc_collect_cycles();
    return $value + W10FpmObject::$destructed - 40;
}
NATIVE_PHP;

$w10 = native_mir_test_compile_execute(
    $w10Source,
    'w10-fpm-native.php',
    [],
    ['wave' => 10, 'function' => 'w10_fpm_outer', 'repeat' => 10],
);
printf(
    "w10=%s execution=%s return=%s runs=%d vm=%d execute_ex=%d handler=%d\n",
    $w10['status'],
    $w10['execution']['status'],
    json_encode($w10['execution']['return_value']),
    $w10['execution']['executions'],
    $w10['execution']['vm_handler_calls'],
    $w10['execution']['execute_ex_calls'],
    $w10['execution']['opline_handler_calls'],
);

class W11FpmStream
{
    public $context;
    private string $source = '';
    private int $offset = 0;

    public function stream_open(
        string $path,
        string $mode,
        int $options,
        ?string &$openedPath,
    ): bool {
        $this->source = '<?php return 22;';
        $this->offset = 0;
        $openedPath = $path;
        return true;
    }

    public function stream_read(int $length): string
    {
        $chunk = substr($this->source, $this->offset, $length);
        $this->offset += strlen($chunk);
        return $chunk;
    }

    public function stream_eof(): bool
    {
        return $this->offset >= strlen($this->source);
    }

    public function stream_stat(): array
    {
        return ['size' => strlen($this->source)];
    }

    public function url_stat(string $path, int $flags): array
    {
        return ['size' => strlen('<?php return 22;')];
    }

    public function stream_set_option(
        int $option,
        int $argument1,
        int $argument2,
    ): bool {
        return false;
    }
}

stream_wrapper_register('w11fpm', W11FpmStream::class);
$w11Source = <<<'NATIVE_PHP'
<?php
function w11_fpm_outer(string $directory): array {
    $previous = set_include_path($directory);
    $trace = [];
    $first = static function (string $class) use (&$trace): void {
        $trace[] = 'first:' . $class;
    };
    $second = static function (string $class) use ($directory, &$trace): void {
        $trace[] = 'second:' . $class;
        if ($class === 'W11FpmChild') {
            include_once $directory . '/child.php';
        } elseif ($class === 'W11FpmBase') {
            include_once $directory . '/base.php';
        }
    };
    spl_autoload_register($first);
    spl_autoload_register($second);
    try {
        $pathValue = include 'path-unit.php';
        $streamValue = include 'w11fpm://dynamic-unit';
        $runtime = eval(<<<'CODE'
interface W11FpmContract {
    public function value(): int;
}
trait W11FpmTrait {
    public function value(): int {
        return W11_FPM_CONSTANT;
    }
}
enum W11FpmEnum: int {
    case Answer = 42;
}
class W11FpmRuntime implements W11FpmContract {
    use W11FpmTrait;
}
const W11_FPM_CONSTANT = 42;
function w11_fpm_static(): int {
    static $calls = 0;
    return ++$calls;
}
return [
    (new W11FpmRuntime())->value(),
    W11FpmEnum::Answer->value,
    w11_fpm_static(),
    w11_fpm_static(),
];
CODE);
        $loaded = (new W11FpmChild())->value();
        $captured = 40;
        $closure = eval(
            'return static function () use (&$captured): int { '
            . 'return ++$captured; };'
        );
        $closureValue = $closure();
        return [
            $pathValue,
            $streamValue,
            $runtime,
            $loaded,
            $closureValue,
            $trace,
        ];
    } finally {
        spl_autoload_unregister($second);
        spl_autoload_unregister($first);
        set_include_path($previous);
    }
}
NATIVE_PHP;

$w11 = native_mir_test_compile_execute(
    $w11Source,
    'w11-fpm-native.php',
    [__DIR__ . '/w11-library'],
    ['wave' => 11, 'function' => 'w11_fpm_outer'],
);
printf(
    "w11=%s execution=%s return=%s worker=%d vm=%d execute_ex=%d handler=%d\n",
    $w11['status'],
    $w11['execution']['status'],
    json_encode($w11['execution']['return_value']),
    getmypid(),
    $w11['execution']['vm_handler_calls'],
    $w11['execution']['execute_ex_calls'],
    $w11['execution']['opline_handler_calls'],
);
stream_wrapper_unregister('w11fpm');
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

run_fpm_request() {
	local output=$1

	SCRIPT_FILENAME=$request \
	SCRIPT_NAME=/request.php \
	REQUEST_URI=/request.php \
	REQUEST_METHOD=GET \
	SERVER_PROTOCOL=HTTP/1.1 \
	GATEWAY_INTERFACE=CGI/1.1 \
	SERVER_SOFTWARE=native-w11-test \
	REMOTE_ADDR=127.0.0.1 \
	REMOTE_PORT=12345 \
	SERVER_ADDR=127.0.0.1 \
	SERVER_PORT=9000 \
		cgi-fcgi -bind -connect "$socket" >"$output"
}

run_fpm_request "$response_one"
run_fpm_request "$response_two"

for response in "$response_one" "$response_two"; do
	grep -F '<native_fpm_outer>' "$response" >/dev/null
	grep -F '<native_fpm_inner>' "$response" >/dev/null
	grep -F '<intdiv>' "$response" >/dev/null
	grep -F '<strcmp>' "$response" >/dev/null
	grep -F '<w09_fpm_outer>' "$response" >/dev/null
	grep -F '<w09_fpm_collect>' "$response" >/dev/null
	grep -F '<w09_fpm_map>' "$response" >/dev/null
	grep -F '<w09_fpm_ref>' "$response" >/dev/null
	grep -F '<array_map>' "$response" >/dev/null
	grep -F '<w10_fpm_outer>' "$response" >/dev/null
	grep -F '<w10_fpm_loaded_callback>' "$response" >/dev/null
	grep -F '<w11_fpm_outer>' "$response" >/dev/null
	grep -F '<w11_fpm_static>' "$response" >/dev/null
	grep -F '<W11FpmStream::stream_open>' "$response" >/dev/null
	grep -F '<W11FpmChild::value>' "$response" >/dev/null
	grep -F \
		'accepted=accepted execution=returned return=42 exception=0 bailout=0 vm=0 execute_ex=0 handler=0' \
		"$response" >/dev/null
	grep -F \
		'w09=accepted execution=returned return=[{"numbers":[1,2,3],"stable":"source"},{"numbers":{"0":2,"1":4,"2":6,"3":7,"4":"named","tail":"unpacked"},"stable":"source"},{"0":2,"1":4,"2":6,"3":7,"4":"named","tail":"unpacked"},["mapped-a","mapped-b"],["catch","finally"],"A2B4C6D",[0,1,2]] runs=10 vm=0 execute_ex=0 handler=0' \
		"$response" >/dev/null
	grep -F \
		'w10=accepted execution=returned return=42 runs=10 vm=0 execute_ex=0 handler=0' \
		"$response" >/dev/null
	grep -F \
		'w11=accepted execution=returned return=[20,22,[42,42,1,2],42,41,["first:W11FpmChild","second:W11FpmChild","first:W11FpmBase","second:W11FpmBase"]]' \
		"$response" >/dev/null
	grep -F 'vm=0 execute_ex=0 handler=0' "$response" >/dev/null
done

worker_one=$(sed -n 's/.*w11=.* worker=\([0-9][0-9]*\) vm=.*/\1/p' \
	"$response_one")
worker_two=$(sed -n 's/.*w11=.* worker=\([0-9][0-9]*\) vm=.*/\1/p' \
	"$response_two")
[[ -n $worker_one && $worker_one == "$worker_two" ]]

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

if timeout 10s "$candidate" -n -d display_errors=1 -d log_errors=0 \
    "$timeout_source" >"$timeout_output" 2>&1; then
    timeout_status=0
else
    timeout_status=$?
fi
[[ $timeout_status == 0 || $timeout_status == 255 ]] || {
    cat "$timeout_output" >&2
    printf 'timeout process exited %d\n' "$timeout_status" >&2
    exit 1
}
grep -F 'Maximum execution time of 1 second exceeded' "$timeout_output" >/dev/null
grep -F \
    'status=error execution=bailout exception=0 bailout=1 vm=0 execute_ex=0 handler=0' \
    "$timeout_output" >/dev/null

printf 'PASS real_fpm=2 same_worker=1 w09_values=1 w10_objects_callables=1 w11_dynamic_code=1 w11_request_teardown=1 internal_calls=4 exception_caught=3 observer=1 timeout_process=1 vm_dispatch=0\n'
