--TEST--
An inline frameless call's exception reaches its catch block or the caller
--DESCRIPTION--
The handler of an inline frameless call (a live execution context, defined
arguments) throws a TypeError: inside try the exception block catches it,
outside the function returns the exception to its caller.
--EXTENSIONS--
native_mir_test
--FILE--
<?php
function h($x) { return "[$x]"; }
function g5($s) {
    $a = str_replace('a', 'b', $s);
    return h($a);
}
function g6($s) {
    try {
        $a = str_replace('a', 'b', $s);
        return h($a);
    } catch (TypeError $e) {
        return h('caught ' . $e->getMessage());
    }
}
for ($i = 0; $i < 3; $i++) {
    $v = $i == 1 ? new stdClass : "aa$i";
    try {
        echo g5($v), "\n";
    } catch (TypeError $e) {
        echo 'outer ', $e->getMessage(), "\n";
    }
    echo g6($v), "\n";
}
--EXPECT--
[bb0]
[bb0]
outer str_replace(): Argument #3 ($subject) must be of type array|string, stdClass given
[caught str_replace(): Argument #3 ($subject) must be of type array|string, stdClass given]
[bb2]
[bb2]
