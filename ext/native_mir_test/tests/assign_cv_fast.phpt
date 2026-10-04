--TEST--
CV assignment without decoding the operation
--DESCRIPTION--
$cv = a defined CV, a temporary or a literal, with an unused or temporary
result, assigns through zend_assign_to_variable_ex() without decoding:
old arrays and objects are released (destructors run), references and
typed references are honoured, and undefined sources still warn.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class D { function __construct(public $n) {} function __destruct() { echo "~{$this->n} "; } }
class T { public int $i = 0; }
function run($k) {
    $a = [new D("a$k")]; $b = [1, 2];
    $a = $b;
    $c = $d = $b;
    $e = str_repeat('x', $k + 1);
    $e = $e;
    $r = 1; $ref = &$r; $ref = [3];
    $t = new T; $ti = &$t->i; try { $ti = 'nope'; } catch (TypeError $x) { echo "TypeError "; } $ti = 5;
    $l = 'lit'; $l = [4, 5];
    return json_encode([$a, $c, $d, $e, $r, $t->i, $l]);
}
for ($i = 0; $i < 3; $i++) { echo run($i), "\n"; }
$z = @$undef; var_dump($z);
$w = $undef2;
?>
--EXPECTF--
~a0 TypeError [[1,2],[1,2],[1,2],"x",[3],5,[4,5]]
~a1 TypeError [[1,2],[1,2],[1,2],"xx",[3],5,[4,5]]
~a2 TypeError [[1,2],[1,2],[1,2],"xxx",[3],5,[4,5]]
NULL

Warning: Undefined variable $undef2 in %s on line 17
