--TEST--
Native UNSET_DIM of array elements without decoding
--DESCRIPTION--
unset($a[k]) of an array CV, or of the array a write fetch's VAR points to,
with an integer or string key deletes without decoding the operation.
Shared arrays, destructors of removed elements, references, missing keys,
strings, null and undefined containers keep stock semantics.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class D { public $n; function __construct($n) { $this->n = $n; } function __destruct() { echo "~{$this->n}|"; } }
class H { public $a = ['x' => 1, 'y' => 2, 5 => 'five']; function run() { unset($this->a['x']); unset($this->a[5]); unset($this->a['nope']); return $this->a; } }
function s($x) { return $x; }
function t($r) {
  $a = ['k' => 1, 'j' => new D("d$r"), '7' => 7, 'l' => [1, 2]]; $b = $a; $ref = &$a;
  unset($a['k']); unset($a['j']); echo "[after unset j]|"; unset($a['7']); unset($a[s('l')][0]); unset($a['missing']); try { unset($this_is_int['x']['y']); } catch (Error $e) {}
  $s = 'str'; try { unset($s[0]); } catch (Error $e) { echo get_class($e), "|"; }
  $n = null; unset($n['x']); unset($undef['x']);
  unset($a[null]);
  return [$a, count($b), (new H)->run()];
}
for ($i = 0; $i < 2; $i++) echo json_encode(t($i)), "|\n";
?>
--EXPECTF--
[after unset j]|
Warning: Undefined variable $this_is_int in %s on line %d
Error|
Warning: Undefined variable $undef in %s on line %d
~d0|[{"l":{"1":2}},4,{"y":2}]|
[after unset j]|
Warning: Undefined variable $this_is_int in %s on line %d
Error|
Warning: Undefined variable $undef in %s on line %d
~d1|[{"l":{"1":2}},4,{"y":2}]|
