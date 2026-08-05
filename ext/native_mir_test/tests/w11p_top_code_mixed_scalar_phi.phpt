--TEST--
Native top-level code transports scalar PHI inputs into canonical zvals
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$result = native_mir_test_compile_execute(
    <<<'PHP'
<?php
$integerLimit = getenv('W11P_INTEGER_LIMIT') ? 3 : 10;
for ($i = 0; $i < $integerLimit; $i++) {
}

$floatLimit = getenv('W11P_FLOAT_LIMIT') ? 3.0 : 10.0;
for ($f = 0.0; $f < $floatLimit; $f += 1.25) {
}

return [$i, $f];
PHP,
    'w11p-top-code-mixed-scalar-phi.php',
    [],
    ['wave' => 11],
);

printf(
    "%s return=%s active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=[10,10] active=0
