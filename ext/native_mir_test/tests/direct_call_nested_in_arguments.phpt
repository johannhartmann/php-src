--TEST--
Native direct call nested in another call's arguments
--DESCRIPTION--
A direct call evaluated while an outer call is pending keeps that call in
EX(call) and restores it on return and on an exception, as the VM's DO does.
--EXTENSIONS--
native_mir_test
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class D {
    function __construct(public $n) {}
    function __destruct() { echo "~{$this->n} "; }
}
function inner($x) {
    if ($x === 7) {
        throw new Exception("inner $x");
    }
    $t = debug_backtrace();
    return $x + count($t);
}
function inner2($x) { return $x * 2; }
function outer($a, $b, $c) { return $a + $b + $c; }
function run($i) {
    return outer(inner2($i), inner($i), inner2(inner($i + 1)));
}
function run_objects($i) {
    return outer(new D("a$i") ? 1 : 0, inner($i), strlen(str_repeat('x', inner2($i))));
}
$sum = 0;
for ($i = 0; $i < 50; $i++) {
    if ($i === 6 || $i === 7) {
        continue;
    }
    $sum += run($i);
}
echo $sum, "\n";
for ($i = 0; $i < 3; $i++) {
    echo run_objects($i), "\n";
}
try {
    run_objects(7);
} catch (Exception $e) {
    echo $e->getMessage(), "\n";
}
try {
    run(6);
} catch (Exception $e) {
    echo $e->getMessage(), "\n";
}
echo "done\n";
?>
--EXPECT--
6444
~a0 3
~a1 6
~a2 9
~a7 inner 7
inner 7
done
