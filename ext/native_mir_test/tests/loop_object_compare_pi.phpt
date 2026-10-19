--TEST--
Native: Pi copies of compared objects in a loop
--DESCRIPTION--
A comparison of two objects in a loop gives the branch Pi copies of both
objects. Their PHI is a register pointer selected later in plan order than
the copies, which must read that register result (the copies once read a
value nothing defined, and compilation aborted).
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
// A comparison of objects puts Pi copies of them into the loop body.
function count_smaller(int $n, object $x, object $y) {
    $c = 0;
    for ($i = 0; $i < $n; $i++) {
        if ($x < $y) { $c++; }
    }
    return $c;
}
function count_dates(int $n) {
    $a = new DateTimeImmutable('2020-01-01');
    $b = new DateTimeImmutable('2021-01-01');
    $c = 0;
    for ($i = 0; $i < $n; $i++) {
        if ($a < $b) { $c++; }
        if ($a == $b) { $c += 100; }
    }
    return $c;
}
var_dump(count_smaller(5, new stdClass, new stdClass));
$p = new stdClass; $p->v = 1; $q = new stdClass; $q->v = 2;
var_dump(count_smaller(5, $p, $q), count_dates(7));
?>
--EXPECT--
int(0)
int(5)
int(7)
