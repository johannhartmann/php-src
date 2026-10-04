--TEST--
Native x64 moves returned temporaries into the caller
--DESCRIPTION--
RETURN of a temporary moves it into the caller's return zval inline.
References, discarded results and by-reference returns keep the helper,
including destructor timing for a discarded object.
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
function rec(int $d): int { return $d === 0 ? 1 : rec($d - 1) + 1; }
function str(int $n) { return $n > 1 ? str_repeat('a', $n) : 'x' . $n; }
function arr(int $n) { return $n ? [$n, $n + 1] : []; }
function obj(int $n) { return $n ? new ArrayObject([$n]) : null; }
function &ref_ret(array &$a) { return $a[0]; }
function mixed_ret($v) { return $v ? $v . '!' : $v; }
class D { public $log = []; function __destruct() { echo "destruct\n"; } }
function discard_obj() { return new D; }
for ($i = 0; $i < 3; $i++) {
    var_dump(rec(4), str(3), str(1), arr(2), arr(0), obj(1)->count(), obj(0));
    $a = [5]; $r = &ref_ret($a); $r++; var_dump($a);
    var_dump(mixed_ret('q'), mixed_ret(0), mixed_ret(null));
    discard_obj();
    echo "after discard\n";
    $x = discard_obj(); unset($x);
}
?>
--EXPECT--
int(5)
string(3) "aaa"
string(2) "x1"
array(2) {
  [0]=>
  int(2)
  [1]=>
  int(3)
}
array(0) {
}
int(1)
NULL
array(1) {
  [0]=>
  &int(6)
}
string(2) "q!"
int(0)
NULL
destruct
after discard
destruct
int(5)
string(3) "aaa"
string(2) "x1"
array(2) {
  [0]=>
  int(2)
  [1]=>
  int(3)
}
array(0) {
}
int(1)
NULL
array(1) {
  [0]=>
  &int(6)
}
string(2) "q!"
int(0)
NULL
destruct
after discard
destruct
int(5)
string(3) "aaa"
string(2) "x1"
array(2) {
  [0]=>
  int(2)
  [1]=>
  int(3)
}
array(0) {
}
int(1)
NULL
array(1) {
  [0]=>
  &int(6)
}
string(2) "q!"
int(0)
NULL
destruct
after discard
destruct
