--TEST--
Native x64 inline call frames with temporary arguments
--DESCRIPTION--
A direct call whose arguments include untyped temporaries, such as
f($m - 1, $n), moves them into an inline callee frame instead of taking
the generic call path. Strings, arrays, objects and their destructors,
defaults and argument errors keep VM behavior.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
class D { public $n; function __construct($n) { $this->n = $n; } function __destruct() { echo "destruct {$this->n}\n"; } }
function take($a, $b = 'def') { return is_object($a) ? 'obj' . $a->n : (is_array($a) ? count($a) : "$a|$b"); }
function rec($m, $n) { if ($m == 0) return $n + 1; return rec($m - 1, $n * 1); }
function mk($n) { return new D($n); }
function deep($x) { return $x > 0 ? deep($x - 1) + $x * 0.5 : 0; }
function refp(&$r) { $r .= '!'; return $r; }
for ($i = 0; $i < 3; $i++) {
    var_dump(take($i + 1), take('s' . $i, $i * 2), take([$i, $i]), take(mk($i)));
    echo "after obj\n";
    var_dump(rec(3, 2.5), rec(2, 7), deep(10), take(str_repeat('ab', 3) . $i));
    $v = 'r'; var_dump(refp($v), $v);
    try { take(); } catch (ArgumentCountError $e) { echo get_class($e), "\n"; }
}
function plus1($x) { return $x + 1; }
var_dump(array_map(plus1(...), range(1, 5)));
function nest($a, $b) { return $a . ':' . $b; }
var_dump(nest(nest(1, 2), nest('a' . 1, 3)));
?>
--EXPECT--
destruct 0
string(5) "1|def"
string(4) "s0|0"
int(2)
string(4) "obj0"
after obj
float(3.5)
int(8)
float(27.5)
string(11) "ababab0|def"
string(2) "r!"
string(2) "r!"
ArgumentCountError
destruct 1
string(5) "2|def"
string(4) "s1|2"
int(2)
string(4) "obj1"
after obj
float(3.5)
int(8)
float(27.5)
string(11) "ababab1|def"
string(2) "r!"
string(2) "r!"
ArgumentCountError
destruct 2
string(5) "3|def"
string(4) "s2|4"
int(2)
string(4) "obj2"
after obj
float(3.5)
int(8)
float(27.5)
string(11) "ababab2|def"
string(2) "r!"
string(2) "r!"
ArgumentCountError
array(5) {
  [0]=>
  int(2)
  [1]=>
  int(3)
  [2]=>
  int(4)
  [3]=>
  int(5)
  [4]=>
  int(6)
}
string(8) "1:2:a1:3"
