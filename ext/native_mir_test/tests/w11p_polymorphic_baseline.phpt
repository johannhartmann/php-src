--TEST--
Native baseline does not specialize a shared entry from observed call-site types
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$source = <<<'PHP'
<?php
function identity(mixed $value): mixed
{
    return $value;
}

function selected(): int
{
    var_dump(identity(1));
    var_dump(identity(1.5));
    var_dump(identity("x"));
    var_dump(identity([1, 2]));
    var_dump(identity((object) ['property' => 3]));
    return 7;
}
PHP;

ob_start();
$result = native_mir_test_compile_execute(
    $source,
    'w11p-polymorphic-baseline.php',
    [],
    ['wave' => 11, 'function' => 'selected'],
);
$output = ob_get_clean();

echo $output;
printf(
    "%s return=%d codeunits=%d vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    $result['execution']['return_value'],
    $result['execution']['native_codeunits'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
);
?>
--EXPECT--
int(1)
float(1.5)
string(1) "x"
array(2) {
  [0]=>
  int(1)
  [1]=>
  int(2)
}
object(stdClass)#1 (1) {
  ["property"]=>
  int(3)
}
accepted return=7 codeunits=3 vm=0 execute_ex=0 handler=0
