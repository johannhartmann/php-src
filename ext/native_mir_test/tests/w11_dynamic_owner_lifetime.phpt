--TEST--
Native MIR W11 keeps dynamic codeunit versions and method scope alive
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$directory = sys_get_temp_dir() . '/native-w11-owners-' . getmypid();
$included = $directory . '/version.php';
mkdir($directory);
file_put_contents($included, <<<'PHP'
<?php
$version = 1;
return static function (int &$calls) use ($version): int {
    static $local = 0;
    $calls++;
    return $version * 100 + ++$local;
};
PHP);

$source = <<<'PHP'
<?php
class W11DynamicScope {
    public function __construct(private int $base) {
    }

    public function run(string $included): array {
        $calls = 0;
        $first = include $included;
        file_put_contents(
            $included,
            <<<'CODE'
<?php
$version = 2;
return static function (int &$calls) use ($version): int {
    static $local = 0;
    $calls++;
    return $version * 100 + ++$local;
};
CODE,
        );
        clearstatcache(true, $included);
        $second = include $included;
        $captured = 1;
        $fromEval = eval(
            'return function (int &$value) use (&$captured): int { '
            . '$value += $this->base; '
            . 'return $value + ++$captured; '
            . '};'
        );
        return [
            $first($calls),
            $second($calls),
            $first($calls),
            $second($calls),
            $fromEval($calls),
            $fromEval($calls),
            $calls,
        ];
    }
}

function w11_dynamic_owner_lifetime(string $included): array {
    return (new W11DynamicScope(10))->run($included);
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11-dynamic-owner-lifetime.php',
    [$included],
    ['wave' => 11, 'function' => 'w11_dynamic_owner_lifetime'],
);
if ($result['status'] !== 'accepted') {
    printf("diagnostics=%s\n", json_encode($result['diagnostics'] ?? null));
}
printf(
    "%s return=%s units=%d vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value'] ?? null),
    $result['execution']['native_codeunits'] ?? -1,
    $result['execution']['vm_handler_calls'] ?? -1,
    $result['execution']['execute_ex_calls'] ?? -1,
    $result['execution']['opline_handler_calls'] ?? -1,
);

$escapeSource = <<<'PHP'
<?php
function w11_dynamic_escape(): array {
    return eval(<<<'CODE'
class W11EscapedObject {
    public function value(): int {
        return 42;
    }
}
return [
    static function (int $value): int {
        static $calls = 0;
        return $value + ++$calls;
    },
    new W11EscapedObject(),
];
CODE);
}
PHP;
$escapedResult = native_mir_test_compile_execute(
    $escapeSource,
    'w11-dynamic-escape.php',
    [],
    ['wave' => 11, 'function' => 'w11_dynamic_escape'],
);
[$escapedClosure, $escapedObject] =
    $escapedResult['execution']['return_value'];
printf(
    "escaped=%s closure=[%d,%d] object=%d vm=%d execute_ex=%d handler=%d\n",
    $escapedResult['status'],
    $escapedClosure(40),
    $escapedClosure(40),
    $escapedObject->value(),
    $escapedResult['execution']['vm_handler_calls'] ?? -1,
    $escapedResult['execution']['execute_ex_calls'] ?? -1,
    $escapedResult['execution']['opline_handler_calls'] ?? -1,
);

$globalSource = <<<'PHP'
<?php
function w11_dynamic_global_escape(): int {
    return eval(<<<'CODE'
class W11GlobalEscapedObject {
    public function value(): int {
        return 42;
    }
}
$GLOBALS['w11_global_closure'] = static fn (int $value): int => $value + 2;
$GLOBALS['w11_global_object'] = new W11GlobalEscapedObject();
return 42;
CODE);
}
PHP;
$globalResult = native_mir_test_compile_execute(
    $globalSource,
    'w11-dynamic-global-escape.php',
    [],
    ['wave' => 11, 'function' => 'w11_dynamic_global_escape'],
);
printf(
    "global=%s return=%d closure=%d object=%d vm=%d execute_ex=%d handler=%d\n",
    $globalResult['status'],
    $globalResult['execution']['return_value'],
    $w11_global_closure(40),
    $w11_global_object->value(),
    $globalResult['execution']['vm_handler_calls'] ?? -1,
    $globalResult['execution']['execute_ex_calls'] ?? -1,
    $globalResult['execution']['opline_handler_calls'] ?? -1,
);
unset($w11_global_closure, $w11_global_object);

unlink($included);
rmdir($directory);
?>
--EXPECTF--
accepted return=[101,201,102,202,16,27,24] units=%d vm=0 execute_ex=0 handler=0
escaped=accepted closure=[41,42] object=42 vm=0 execute_ex=0 handler=0
global=accepted return=42 closure=42 object=42 vm=0 execute_ex=0 handler=0
