--TEST--
Native rope conversion exceptions preserve later dynamic-name operands
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
${''} = 42;
$messages = [];
$thrown = 0;
$str = 'a';
$name = new class {
    public function __toString(): string
    {
        throw new Exception((string) ++$GLOBALS['thrown']);
    }
};

try { $value = "x$name$str"; }
catch (Exception $exception) { $messages[] = $exception->getMessage(); }
try { $value = "x$str$name"; }
catch (Exception $exception) { $messages[] = $exception->getMessage(); }
try {
    unset(${$name});
} catch (Exception $exception) {
    $messages[] = $exception->getMessage();
}
return [$messages, ${''}];
PHP,
    'w11p-dynamic-unset-exception-route.php',
    [],
    [
        'wave' => 11,
    ],
);
printf(
    "%s return=%s vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=[["1","2","3"],42] vm=0 execute_ex=0 handler=0 active=0
