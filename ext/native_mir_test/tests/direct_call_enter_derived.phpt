--TEST--
Native direct user calls enter through descriptors validated when built
--DESCRIPTION--
zend_native_call_direct_enter() takes the descriptor's validity and its
trivial-frame shape from flags derived when the descriptor is built or
loaded: untyped, typed, defaulted, variadic, by-reference, instance and
static callees, and argument errors.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function add($a, $b) { return $a + $b; }
function typed(int $a, int $b = 2): int { return $a * $b; }
function locals($a) { $x = $a . '!'; return $x; }
function variadic($a, ...$rest) { return $a + count($rest); }
function defaults($a, $b = [1, 2]) { return $a + count($b); }
function byref(&$a) { $a++; }
class K {
    public $v = 1;
    function m($x) { return $this->v + $x; }
    static function s($x) { return static::class . $x; }
    function run() { return [$this->m(2), self::s(3), static::s(4)]; }
}
class L extends K {}
function drive($n) {
    $sum = 0; $s = '';
    $r = 0;
    for ($i = 0; $i < $n; $i++) {
        $sum += add($i, 1);
        $sum += typed($i);
        $sum += typed($i, 3);
        $s = locals($i);
        $sum += variadic($i, 1, 2);
        $sum += defaults($i);
        byref($r);
        add($i, $i);
    }
    return [$sum, $s, $r, (new K)->run(), (new L)->run()];
}
var_dump(drive(5));
try { typed("x"); } catch (TypeError $e) { echo $e->getMessage(), "\n"; }
try { add(1); } catch (ArgumentCountError $e) { echo $e->getMessage(), "\n"; }
?>
--EXPECTF--
array(5) {
  [0]=>
  int(105)
  [1]=>
  string(2) "4!"
  [2]=>
  int(5)
  [3]=>
  array(3) {
    [0]=>
    int(3)
    [1]=>
    string(2) "K3"
    [2]=>
    string(2) "K4"
  }
  [4]=>
  array(3) {
    [0]=>
    int(3)
    [1]=>
    string(2) "L3"
    [2]=>
    string(2) "L4"
  }
}
typed(): Argument #1 ($a) must be of type int, string given, called in %s on line 31
Too few arguments to function add(), 1 passed in %s on line 32 and exactly 2 expected
