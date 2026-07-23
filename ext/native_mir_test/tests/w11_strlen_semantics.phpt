--TEST--
Native MIR preserves source-backed ZEND_STRLEN semantics
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$cases = [
    [
        <<<'PHP'
<?php
function w11_strlen_weak($number, $null): array {
    $string = 'four';
    $reference =& $string;
    $notices = [];
    set_error_handler(
        static function (int $severity, string $message) use (&$notices): bool {
            $notices[] = [$severity, $message];
            return true;
        },
    );
    try {
        $nullLength = strlen($null);
    } finally {
        restore_error_handler();
    }
    return [
        strlen($reference),
        strlen($number),
        strlen(false),
        strlen(new class {
            public function __toString(): string {
                return 'object';
            }
        }),
        $nullLength,
        $notices,
    ];
}
PHP,
        'w11_strlen_weak',
        [12345, null],
    ],
    [
        <<<'PHP'
<?php
declare(strict_types=1);
function w11_strlen_strict($value): array {
    try {
        strlen($value);
    } catch (TypeError $error) {
        return [$error::class, $error->getMessage()];
    }
    return [];
}
PHP,
        'w11_strlen_strict',
        [42],
    ],
];

foreach ($cases as $index => [$source, $function, $arguments]) {
    $result = native_mir_test_compile_execute(
        $source,
        "w11-strlen-$index.php",
        $arguments,
        ['wave' => 11, 'function' => $function],
    );
    printf(
        "%s %s return=%s vm=%d execute_ex=%d handler=%d\n",
        $function,
        $result['status'],
        json_encode($result['execution']['return_value'] ?? null),
        $result['execution']['vm_handler_calls'] ?? -1,
        $result['execution']['execute_ex_calls'] ?? -1,
        $result['execution']['opline_handler_calls'] ?? -1,
    );
}
?>
--EXPECT--
w11_strlen_weak accepted return=[4,5,0,6,0,[[8192,"strlen(): Passing null to parameter #1 ($string) of type string is deprecated"]]] vm=0 execute_ex=0 handler=0
w11_strlen_strict accepted return=["TypeError","strlen(): Argument #1 ($string) must be of type string, int given"] vm=0 execute_ex=0 handler=0
