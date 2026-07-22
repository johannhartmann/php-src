--TEST--
Native MIR W09 executes named, default, extra, variadic, by-reference, and wide arguments
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$arguments = implode(', ', range(1, 128));
$source = <<<PHP
<?php
function callee(\$a, \$b = 2, ...\$rest) { return [\$a, \$b, \$rest]; }
function bump(&\$value, \$delta = 1) { \$value += \$delta; return \$value; }
function wide(...\$values) { return [\$values[0], \$values[64], \$values[127]]; }
function extra(\$value) { return \$value; }
function argument_case() {
    \$value = 3;
    \$default = callee(a: 8);
    \$named = callee(b: 4, a: 3, c: 5);
    \$changed = bump(value: \$value, delta: 4);
    \$extra = extra(9, 10, 11);
    \$wide = wide($arguments);
    \$internal = strcmp(string1: "a", string2: "b");
    return [\$default, \$named, \$value, \$changed, \$extra, \$wide, \$internal];
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w09-arguments.php',
    [],
    ['wave' => 9, 'function' => 'argument_case'],
);
printf(
    "%s %s return=%s vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    $result['execution']['status'],
    json_encode($result['execution']['return_value']),
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
);
?>
--EXPECT--
accepted returned return=[[8,2,[]],[3,4,{"c":5}],7,7,9,[1,65,128],-1] vm=0 execute_ex=0 handler=0
