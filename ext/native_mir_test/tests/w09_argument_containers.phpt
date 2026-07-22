--TEST--
Native MIR W09 executes unpack, SEND_ARRAY, SEND_USER, named, wide, and by-reference argument containers
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$wide = range(1, 160);
$cases = [
    'array_then_internal' => [
        <<<'PHP'
<?php
function collect($head = 0, ...$values) { return [$head, $values]; }
function argument_container_case() {
    $array = collect(0, ...[1, 2]);
    $internal = strcmp(...['string1' => 'a', 'string2' => 'b']);
    return [$array, $internal];
}
PHP,
        [],
    ],
    'named_array' => [
        <<<'PHP'
<?php
function collect($head = 0, ...$values) { return [$head, $values]; }
function argument_container_case() {
    return collect(...['head' => 4, 'tail' => 5]);
}
PHP,
        [],
    ],
    'traversable' => [
        <<<'PHP'
<?php
function collect($head = 0, ...$values) { return [$head, $values]; }
function argument_container_case($values) {
    return collect(...$values);
}
PHP,
        [new ArrayIterator([11, 12])],
    ],
    'by_reference' => [
        <<<'PHP'
<?php
function bump(&$value) { $value += 10; return $value; }
function argument_container_case($values) {
    $before = $values[0];
    $result = bump(...$values);
    return [$before, $result, $values[0]];
}
PHP,
        [[1]],
    ],
    'send_array_named' => [
        <<<'PHP'
<?php
function collect($head = 0, ...$values) { return [$head, $values]; }
function argument_container_case() {
    return call_user_func_array('collect', ['head' => 4, 'tail' => 5]);
}
PHP,
        [],
    ],
    'send_array_slice' => [
        <<<'PHP'
<?php
function collect($head = 0, ...$values) { return [$head, $values]; }
function argument_container_case() {
    return call_user_func_array('collect', array_slice([6, 7, 8, 9], 1, 2));
}
PHP,
        [],
    ],
    'wide_unpack' => [
        <<<'PHP'
<?php
function wide(...$values) {
    return [$values[0], $values[64], $values[128], $values[159]];
}
function argument_container_case($values) {
    return wide(...$values);
}
PHP,
        [$wide],
    ],
    'user_unpack_type_error' => [
        <<<'PHP'
<?php
function collect($head = 0, ...$values) { return [$head, $values]; }
function argument_container_case($value) {
    try {
        collect(...$value);
    } catch (TypeError) {
        return ['user-caught', collect(9)];
    }
    return 'missed';
}
PHP,
        [42],
    ],
    'internal_unpack_type_error' => [
        <<<'PHP'
<?php
function argument_container_case($value) {
    try {
        strcmp(...$value);
    } catch (TypeError) {
        return ['internal-caught', strcmp('a', 'a')];
    }
    return 'missed';
}
PHP,
        [42],
    ],
];

foreach ($cases as $name => [$source, $arguments]) {
    $result = native_mir_test_compile_execute(
        $source,
        "w09-argument-container-$name.php",
        $arguments,
        ['wave' => 9, 'function' => 'argument_container_case'],
    );
    printf(
        "%s %s %s return=%s vm=%d execute_ex=%d handler=%d\n",
        $name,
        $result['status'],
        $result['execution']['status'],
        json_encode($result['execution']['return_value']),
        $result['execution']['vm_handler_calls'],
        $result['execution']['execute_ex_calls'],
        $result['execution']['opline_handler_calls'],
    );
}
?>
--EXPECT--
array_then_internal accepted returned return=[[0,[1,2]],-1] vm=0 execute_ex=0 handler=0
named_array accepted returned return=[4,{"tail":5}] vm=0 execute_ex=0 handler=0
traversable accepted returned return=[11,[12]] vm=0 execute_ex=0 handler=0
by_reference accepted returned return=[1,11,11] vm=0 execute_ex=0 handler=0
send_array_named accepted returned return=[4,{"tail":5}] vm=0 execute_ex=0 handler=0
send_array_slice accepted returned return=[7,[8]] vm=0 execute_ex=0 handler=0
wide_unpack accepted returned return=[1,65,129,160] vm=0 execute_ex=0 handler=0
user_unpack_type_error accepted returned return=["user-caught",[9,[]]] vm=0 execute_ex=0 handler=0
internal_unpack_type_error accepted returned return=["internal-caught",0] vm=0 execute_ex=0 handler=0
