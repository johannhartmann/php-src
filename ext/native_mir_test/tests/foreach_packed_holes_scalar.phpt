--TEST--
Native foreach over a packed array with holes into an integer destination
--DESCRIPTION--
The inline FE_FETCH_R of a scalar destination leaves a hole of a packed
array to the helper through its integer test, which must skip it.
--EXTENSIONS--
native_mir_test
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function sum(array $a): int {
    $s = 0;
    foreach ($a as $v) {
        $s += $v;
    }
    return $s;
}
function sum_keys(array $a): int {
    $s = 0;
    foreach ($a as $k => $v) {
        $s += $k * $v;
    }
    return $s;
}
$t = 0;
for ($i = 0; $i < 40; $i++) {
    $a = range(1, 10);
    unset($a[1], $a[5]);
    $a[] = 100;
    $t += sum($a) + sum_keys($a) + sum([]) + sum([7]);
}
var_dump($t);
?>
--EXPECT--
int(58080)
