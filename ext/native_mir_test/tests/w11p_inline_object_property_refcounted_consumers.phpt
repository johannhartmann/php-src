--TEST--
Native boxed refcounted property reads compose without frame materialization
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
final class RefcountedPropertyConsumerBox
{
    public mixed $text = 'tpde';
    public mixed $emptyText = '';
    public mixed $items = [1, 2, 3];
    public mixed $child;

    public function __construct()
    {
        $this->child = (object) ['id' => 7];
    }
}

function refcounted_property_consumers(): array
{
    $box = new RefcountedPropertyConsumerBox();
    $truthy = $box->text ? 'yes' : 'no';
    $notText = !$box->text;
    $notEmptyText = !$box->emptyText;
    $upper = strtoupper($box->text);
    $count = count($box->items);
    $text = $box->text;
    $child = $box->child;
    unset($box);

    return [$truthy, $notText, $notEmptyText, $upper, $count, $text, $child->id];
}
PHP,
    'w11p-inline-object-property-refcounted-consumers.php',
    [],
    [
        'wave' => 11,
        'function' => 'refcounted_property_consumers',
        'repeat' => 30,
    ],
);
printf(
    "%s return=%s runs=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    $result['execution']['executions'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=["yes",false,true,"TPDE",3,"tpde",7] runs=30 vm=0 execute_ex=0 handler=0 active=0
