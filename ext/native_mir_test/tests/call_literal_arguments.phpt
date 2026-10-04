--TEST--
Native direct calls pass scalar literal arguments and defaults as immediates
--EXTENSIONS--
native_mir_test
--FILE--
<?php
function callee($a, $b = 5, $c = -7, $d = 1.5, $e = null, $f = true, $g = "s", $h = [1], $i = PHP_INT_MAX) {
    return [$a, $b, $c, $d, $e, $f, $g, $h, $i];
}
function caller() {
    $r = [];
    for ($k = 0; $k < 3; $k++) {
        $r[] = callee(1);
        $r[] = callee(2, 3, 4);
        $r[] = callee(0.25, false, null, -2, 9223372036854775807, 1e300, "x", [], -1);
    }
    return $r;
}
echo md5(serialize(caller())), "\n";
var_dump(callee(1)[3], callee(1)[8]);
?>
--EXPECT--
bd5de75f219b687036901c65fa3c8d1b
float(1.5)
int(9223372036854775807)
