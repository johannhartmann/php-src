--TEST--
Bug #78015: Incorrect evaluation of expressions involving partials array in SCCP
--FILE--
<?php

$x = 1;

function test1() {
    global $x;
    $a = ['b' => [$x], 'c' => [$x]];
    $d = $a['b'] + $a['c'];
    return $d;
}

function test2() {
    global $x;
    $a = ['b' => [$x]];
    $d = !$a['b'];
    return $d;
}

function test3() {
    global $x;
    $a = ['b' => [$x]];
    $d = (int) $a['b'];
    return $d;
}

function test4() {
    global $x;
    $a = ['b' => [$x]];
    $d = $a['b'] ?: 42;
    return $d;
}

function test5() {
    global $x;
    $a = ['b' => [$x]];
    $d = is_array($a['b']);
    return $d;
}

function test6() {
    global $x;
    $a = ['b' => [$x]];
    $b = "foo";
    $d = "$a[b]{$b}bar";
    return $d;
}

var_dump(test1());
var_dump(test2());
var_dump(test3());
var_dump(test4());
var_dump(test5());
var_dump(test6());

?>
--EXPECTF--
array(1) {
  [0]=>
  int(1)
}
bool(false)
int(1)
int(42)
bool(true)

Notice: Array to string conversion in %s on line %d
string(11) "Arrayfoobar"
