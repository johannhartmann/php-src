--TEST--
Native forms consume shared and sole-owner temporaries and copy register scalars
--DESCRIPTION--
=== against a literal, array_key_exists(), count(), is_*() and foreach
consume a temporary another owner keeps alive with one reference drop and
leave a last owner (whose destructors run at the VM's point) to the helper;
assignments of register longs, doubles and booleans need no type tests.
--EXTENSIONS--
native_mir_test
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class D { public $n; function __construct($n) { $this->n = $n; } function __destruct() { echo "D{$this->n} "; } }
function shared_arr() { static $a = [1, 2, 3, 'k' => 4]; return $a; }
function fresh_arr($n) { return [$n, $n + 1, 'k' => new D($n)]; }
function shared_str() { static $s = 'abc'; return $s; }
function fresh_str($n) { return str_repeat('x', $n); }
function run() {
    $out = [];
    for ($i = 0; $i < 3; $i++) {
        $out[] = var_export(shared_arr() === [1, 2, 3, 'k' => 4], true);
        $out[] = var_export(fresh_str($i) === 'xx', true);
        $out[] = var_export(shared_str() !== 'abc', true);
        $out[] = var_export(array_key_exists('k', shared_arr()), true);
        $out[] = var_export(array_key_exists('k', fresh_arr($i)), true);
        $out[] = count(shared_arr()) . '/' . count(fresh_arr($i));
        $out[] = var_export(is_array(fresh_arr($i)), true);
        $out[] = var_export(is_string(shared_str()), true);
        $sum = 0;
        foreach (shared_arr() as $v) { $sum += $v; }
        foreach (fresh_arr($i) as $k => $v) { if (is_int($v)) { $sum += $v; } }
        $out[] = $sum;
        $x = $i * 2 + 1;
        $y = $x;
        $z = $i > 1;
        $w = $z;
        $f = $i / 2;
        $g = $f;
        $out[] = var_export([$y, $w, $g], true);
    }
    return implode("\n", $out);
}
echo run(), "\n";
--EXPECT--
D0 D0 D0 D1 D1 D1 D0 D2 D2 D2 D1 D2 true
false
false
true
true
4/3
true
true
11
array (
  0 => 1,
  1 => false,
  2 => 0,
)
true
false
false
true
true
4/3
true
true
13
array (
  0 => 3,
  1 => false,
  2 => 0.5,
)
true
true
false
true
true
4/3
true
true
15
array (
  0 => 5,
  1 => true,
  2 => 1,
)
