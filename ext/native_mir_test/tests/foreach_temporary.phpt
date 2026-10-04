--TEST--
Native FE_RESET_R of a temporary array
--DESCRIPTION--
foreach over a temporary array, such as a function result, moves the
array into the iterator as FE_RESET_R does. Nested loops, break, writes to
the source variable, empty and shared arrays, destructors of elements and
non-array temporaries keep stock semantics.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class E { public $n; function __construct($n) { $this->n = $n; } function __destruct() { echo "~{$this->n} "; } }
function items($n) { $r = []; for ($i = 0; $i < $n; $i++) { $r["k$i"] = $i * 10; } return $r; }
function objects($r) { return [new E("a$r"), new E("b$r")]; }
function gen() { yield 1; yield 2; }
function run($r) {
    $out = [];
    foreach (items(3) as $k => $v) { foreach (items(2) as $k2 => $v2) { $out[] = "$k$k2=" . ($v + $v2); } }
    foreach (items(5) as $k => $v) { if ($v > 10) break; $out[] = $k; }
    $shared = items(2);
    foreach (array_merge($shared, ['x' => 1]) as $k => $v) { $shared[$k] = $v + 1; }
    $out[] = json_encode($shared);
    foreach (items(0) as $v) { $out[] = 'never'; }
    foreach (objects($r) as $o) { $out[] = $o->n; }
    echo "[after objects] ";
    foreach (gen() as $g) { $out[] = $g; }
    foreach (new ArrayIterator([7, 8]) as $a) { $out[] = $a; }
    return $out;
}
for ($i = 0; $i < 3; $i++) { echo json_encode(run($i)), "|\n"; }
?>
--EXPECT--
~a0 [after objects] ~b0 ["k0k0=0","k0k1=10","k1k0=10","k1k1=20","k2k0=20","k2k1=30","k0","k1","{\"k0\":1,\"k1\":11,\"x\":2}","a0","b0",1,2,7,8]|
~a1 [after objects] ~b1 ["k0k0=0","k0k1=10","k1k0=10","k1k1=20","k2k0=20","k2k1=30","k0","k1","{\"k0\":1,\"k1\":11,\"x\":2}","a1","b1",1,2,7,8]|
~a2 [after objects] ~b2 ["k0k0=0","k0k1=10","k1k0=10","k1k1=20","k2k0=20","k2k1=30","k0","k1","{\"k0\":1,\"k1\":11,\"x\":2}","a2","b2",1,2,7,8]|
