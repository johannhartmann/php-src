--TEST--
Native array offset errors preserve Zend access context
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
function w11p_array_offset_error_context(object $key): array
{
    $array = [];
    $messages = [];
    try {
        $value = $array[$key];
    } catch (TypeError $error) {
        $messages[] = $error->getMessage();
    }
    try {
        $array[$key] = 1;
    } catch (TypeError $error) {
        $messages[] = $error->getMessage();
    }
    try {
        unset($array[$key]);
    } catch (TypeError $error) {
        $messages[] = $error->getMessage();
    }
    try {
        isset($array[$key]);
    } catch (TypeError $error) {
        $messages[] = $error->getMessage();
    }
    try {
        array_key_exists($key, $array);
    } catch (TypeError $error) {
        $messages[] = $error->getMessage();
    }
    try {
        $array = [new stdClass() => null];
    } catch (TypeError $error) {
        $messages[] = $error->getMessage();
    }
    return $messages;
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-array-offset-error-context.php',
    [new stdClass()],
    [
        'wave' => 11,
        'function' => 'w11p_array_offset_error_context',
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
accepted return=["Cannot access offset of type stdClass on array","Cannot access offset of type stdClass on array","Cannot unset offset of type stdClass on array","Cannot access offset of type stdClass in isset or empty","Cannot access offset of type stdClass on array","Cannot access offset of type stdClass on array"] vm=0 execute_ex=0 handler=0 active=0
