--TEST--
Native CHECK_FUNC_ARG decides by-reference sends of positional arguments
--EXTENSIONS--
native_mir_test
--FILE--
<?php
function by_value($a, $b) { return "$a|$b"; }
function by_ref(&$a, $b) { $a[] = $b; return count($a); }
function by_ref_second($a, &$b) { $b = "$a!"; return $a; }
function by_ref_thirteenth($a1, $a2, $a3, $a4, $a5, $a6, $a7, $a8, $a9, $a10, $a11, $a12, &$a13) { $a13 = 13; return $a1; }
function prefer_ref(...$args) { return count($args); }
$data = ['x' => [1], 'y' => 'two'];
$o = new stdClass; $o->list = [];
for ($i = 0; $i < 3; $i++) {
    $f = $i % 2 ? 'by_value' : 'by_ref';
    echo $f($data['x'], $i), "\n";
    echo by_ref_second($data['y'], $data['z']), ' ', $data['z'], "\n";
    echo by_ref($o->list, $i), ' ', json_encode($o->list), "\n";
    echo by_ref_thirteenth(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, $data['t']), ' ', $data['t'], "\n";
    $g = $i == 2 ? 'by_ref_second' : 'by_value';
    echo $g($data['y'], $data['w']), ' ', json_encode($data['w'] ?? null), "\n";
    echo prefer_ref($data['x'], $data['y']), "\n";
}
echo json_encode($data), "\n";
--EXPECTF--
2
two two!
1 [0]
1 13

Warning: Undefined array key "w" in %s on line 16
two| null
2

Warning: Array to string conversion in %s on line 2
Array|1
two two!
2 [0,1]
1 13

Warning: Undefined array key "w" in %s on line 16
two| null
2
3
two two!
3 [0,1,2]
1 13
two "two!"
2
{"x":[1,0,2],"y":"two","z":"two!","t":13,"w":"two!"}
