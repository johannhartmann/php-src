--TEST--
Native write and read-write array fetches of array CVs
--DESCRIPTION--
FETCH_DIM_W and FETCH_DIM_RW of an integer or string key of an array CV
separate the array, find the element (a write inserts a missing key as
null) and publish it INDIRECT without decoding the operation. A missing
key of a read-write fetch keeps the general path and its warning.
--EXTENSIONS--
native_mir_test
--FILE--
<?php
function run($n) {
    $counts = [];
    $nested = ['a' => ['x' => 1]];
    $shared = ['k' => 1];
    $copy = $shared;
    $out = [];
    for ($i = 0; $i < $n; $i++) {
        $k = "h" . ($i % 2);
        if (!isset($counts[$k])) { $counts[$k] = 1; } else { ++$counts[$k]; }
        $counts[7]++;
        $counts["12"] += 2;
        $nested['a']['y'][] = $i;
        $nested[$k]['z'] = $i;
        $copy['k']++;
        $ref = &$counts['r'];
        $ref = $i;
        unset($ref);
    }
    $u = []; @$u['missing']++;
    $out[] = json_encode($counts) . ' ' . json_encode($nested) . ' ' . json_encode($shared) . json_encode($copy) . json_encode($u);
    return implode("\n", $out);
}
echo run(4), "\n";
?>
--EXPECTF--

Warning: Undefined array key 7 in %s on line 11

Warning: Undefined array key 12 in %s on line 12
{"h0":2,"7":4,"12":8,"r":3,"h1":2} {"a":{"x":1,"y":[0,1,2,3]},"h0":{"z":2},"h1":{"z":3}} {"k":1}{"k":5}{"missing":1}
