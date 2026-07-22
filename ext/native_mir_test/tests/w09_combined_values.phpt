--TEST--
Native MIR W09 executes combined value, container, call, and lifetime semantics
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
function w09_recursive_values($values, $depth)
{
    if ($depth === 0) {
        return $values;
    }
    $copy = $values;
    $copy[] = $depth;
    return w09_recursive_values($copy, $depth - 1);
}

function w09_collect_values(&$values, $label = 'default', ...$extra)
{
    $values[] = $label;
    foreach ($extra as $key => $value) {
        $values[$key] = $value;
    }
    return $values;
}

function w09_map_value($value)
{
    return "mapped-$value";
}

function combined_value_case($input, $iterator, $object, $resource, $branch)
{
    $original = $input;
    $numbers =& $input['numbers'];
    foreach ($numbers as &$number) {
        $number *= 2;
    }
    unset($number);

    $callArguments = ['label' => 'named', 'tail' => 'unpacked'];
    $called = w09_collect_values($numbers, ...$callArguments);
    $mapped = array_map('w09_map_value', ['a', 'b']);

    $iterated = [];
    foreach ($iterator as $key => $value) {
        $iterated[$key] = $value;
    }

    $events = [];
    try {
        intdiv(1, 0);
    } catch (DivisionByZeroError) {
        $events[] = 'catch';
    } finally {
        $events[] = 'finally';
    }

    $rope = "A{$numbers[0]}B{$numbers[1]}C{$numbers[2]}D";
    $ropeAlias = $rope;
    $rope[0] = 'Z';
    $last = $rope[-1];
    $ropeError = false;
    try {
        unset($rope[1]);
    } catch (Error) {
        $ropeError = true;
    }
    $replacements = 0;
    $replaced = str_replace('B', 'b', $rope, $replacements);

    $packed = [10, 20];
    $packed['name'] = 'mixed';
    $packed[] = 30;
    unset($packed[0]);
    $packed[0] = 40;

    $keys = [];
    $keys[true] = 'bool';
    $keys[null] = 'null';
    $keys[2.9] = 'float';
    try {
        $keys[[]] = 'invalid';
    } catch (TypeError) {
        $keys['invalid'] = 'TypeError';
    }

    $cycle = [];
    $cycle['self'] =& $cycle;
    $cycle['value'] = 7;
    $cycleValid = $cycle === $cycle['self'];
    unset($cycle);
    $gcRan = gc_collect_cycles() >= 0;

    if ($branch) {
        $selected = $input;
    } else {
        $selected = $original;
    }
    for ($index = 0; $index < 3; $index++) {
        $selected['loop'][] = $index;
    }

    return [
        'original' => $original,
        'input' => $input,
        'called' => $called,
        'recursive' => w09_recursive_values(['root'], 3),
        'mapped' => $mapped,
        'iterator' => $iterated,
        'events' => $events,
        'rope' => [$rope, $ropeAlias, $last, $ropeError, $replaced, $replacements],
        'packed' => $packed,
        'keys' => $keys,
        'cycle' => [$cycleValid, $gcRan],
        'selected' => $selected,
        'opaque' => [get_debug_type($object), get_resource_type($resource)],
    ];
}
PHP;

$input = ['numbers' => [1, 2, 3], 'stable' => 'source'];
$iterator = new ArrayIterator(['first' => 4, 'second' => 5]);
$object = (object) ['opaque' => true];
$resource = fopen('php://memory', 'w+');
error_reporting(E_ALL & ~E_DEPRECATED);

foreach ([false, true] as $branch) {
    $result = native_mir_test_compile_execute(
        $source,
        'w09-combined-values.php',
        [$input, $iterator, $object, $resource, $branch],
        ['wave' => 9, 'function' => 'combined_value_case', 'repeat' => 10],
    );
    printf(
        "%s %s return=%s runs=%d vm=%d execute_ex=%d handler=%d\n",
        $result['status'],
        $result['execution']['status'] ?? '-',
        json_encode($result['execution']['return_value'] ?? null),
        $result['execution']['executions'] ?? -1,
        $result['execution']['vm_handler_calls'] ?? -1,
        $result['execution']['execute_ex_calls'] ?? -1,
        $result['execution']['opline_handler_calls'] ?? -1,
    );
}
fclose($resource);
?>
--EXPECT--
accepted returned return={"original":{"numbers":[1,2,3],"stable":"source"},"input":{"numbers":{"0":2,"1":4,"2":6,"3":"named","tail":"unpacked"},"stable":"source"},"called":{"0":2,"1":4,"2":6,"3":"named","tail":"unpacked"},"recursive":["root",3,2,1],"mapped":["mapped-a","mapped-b"],"iterator":{"first":4,"second":5},"events":["catch","finally"],"rope":["Z2B4C6D","A2B4C6D","D",true,"Z2b4C6D",1],"packed":{"1":20,"name":"mixed","2":30,"0":40},"keys":{"1":"bool","":"null","2":"float","invalid":"TypeError"},"cycle":[true,true],"selected":{"numbers":[1,2,3],"stable":"source","loop":[0,1,2]},"opaque":["stdClass","stream"]} runs=10 vm=0 execute_ex=0 handler=0
accepted returned return={"original":{"numbers":[1,2,3],"stable":"source"},"input":{"numbers":{"0":2,"1":4,"2":6,"3":"named","tail":"unpacked"},"stable":"source"},"called":{"0":2,"1":4,"2":6,"3":"named","tail":"unpacked"},"recursive":["root",3,2,1],"mapped":["mapped-a","mapped-b"],"iterator":{"first":4,"second":5},"events":["catch","finally"],"rope":["Z2B4C6D","A2B4C6D","D",true,"Z2b4C6D",1],"packed":{"1":20,"name":"mixed","2":30,"0":40},"keys":{"1":"bool","":"null","2":"float","invalid":"TypeError"},"cycle":[true,true],"selected":{"numbers":{"0":2,"1":4,"2":6,"3":"named","tail":"unpacked"},"stable":"source","loop":[0,1,2]},"opaque":["stdClass","stream"]} runs=10 vm=0 execute_ex=0 handler=0
