--TEST--
Native MIR W08 publishes usable unwind metadata and tears it down repeatedly
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$walkSource = <<<'PHP'
<?php
function native_unwind_walk(): int
{
    return native_mir_test_unwind_probe();
}
PHP;

$walk = native_mir_test_compile_execute(
    $walkSource,
    'w08-unwind-walk.php',
    [],
    ['wave' => 8, 'function' => 'native_unwind_walk'],
);
printf(
    "%s %s registered=%d native_frames=%d vm=%d execute_ex=%d handler=%d\n",
    $walk['status'],
    $walk['execution']['status'],
    $walk['execution']['unwind_registered'],
    $walk['execution']['return_value'] > 0,
    $walk['execution']['vm_handler_calls'],
    $walk['execution']['execute_ex_calls'],
    $walk['execution']['opline_handler_calls'],
);

$lifecycleSource = <<<'PHP'
<?php
function native_unwind_lifecycle(string $value): int
{
    return strcmp($value, 'ok');
}
PHP;
$registered = 0;
$returned = 0;
$vmCalls = 0;
$executeExCalls = 0;
$handlerCalls = 0;
for ($iteration = 0; $iteration < 50; $iteration++) {
    $result = native_mir_test_compile_execute(
        $lifecycleSource,
        'w08-unwind-lifecycle.php',
        ['ok'],
        ['wave' => 8, 'function' => 'native_unwind_lifecycle'],
    );
    $registered += (int) $result['execution']['unwind_registered'];
    $returned += (int) (
        $result['status'] === 'accepted'
        && $result['execution']['status'] === 'returned'
        && $result['execution']['return_value'] === 0
    );
    $vmCalls += $result['execution']['vm_handler_calls'];
    $executeExCalls += $result['execution']['execute_ex_calls'];
    $handlerCalls += $result['execution']['opline_handler_calls'];
}
printf(
    "iterations=50 registered=%d returned=%d vm=%d execute_ex=%d handler=%d\n",
    $registered,
    $returned,
    $vmCalls,
    $executeExCalls,
    $handlerCalls,
);
?>
--EXPECT--
accepted returned registered=1 native_frames=1 vm=0 execute_ex=0 handler=0
iterations=50 registered=50 returned=50 vm=0 execute_ex=0 handler=0
