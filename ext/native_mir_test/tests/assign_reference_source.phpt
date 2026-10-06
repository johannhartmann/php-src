--TEST--
Assignments from CVs bound by reference (global, static, &)
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class D { public $n; function __construct($n) { $this->n = $n; } function __destruct() { echo "d{$this->n}\n"; } }
function f() {
    global $g;
    $s = $g; $s = $g; echo $s, "\n";
    $a = 'x' . mt_rand(1, 1); $b = &$a; $a = $b; echo $a, "\n"; $b = $a; echo $b, "\n";
    $o = new D(1); $r = &$o; $o = $r; $x = $r; $r = null; echo gettype($x), "\n"; unset($x); echo "after\n";
    static $st = [1, 2]; $c = $st; $c[] = 3; echo count($st), count($c), "\n";
    $u = &$undefined; $v = $u; var_dump($v);
    $arr = [str_repeat('q', 3)]; $e = &$arr[0]; $w = $e; $e = 'z'; echo $w, $arr[0], "\n";
}
$g = str_repeat('glob', 2);
f(); f();
--EXPECT--
globglob
x1
x1
object
d1
after
23
NULL
qqqz
globglob
x1
x1
object
d1
after
23
NULL
qqqz
