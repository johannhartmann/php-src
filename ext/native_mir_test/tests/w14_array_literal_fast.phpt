--TEST--
Native array literal elements without decoding
--DESCRIPTION--
INIT_ARRAY and ADD_ARRAY_ELEMENT add CV, temporary and literal values
under the next index, integer keys and string keys without decoding the
operation. By-reference elements, float, bool and null keys, objects and
nested literals keep stock semantics.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class D { public $n; function __construct($n) { $this->n = $n; } function __destruct() { echo "~{$this->n} "; } }
function s($x) { return $x; }
function t($r) {
  $a = 'x'; $b = [1]; $o = new D("d$r"); $ref = &$a;
  $l = [$a, $b, $o, s('t'), 'lit', 5, null];
  $m = ['k' => $a, '5' => 'five', 7 => s(7), s('dyn') => $b, 'nested' => ['x' => $r], $r => 'r', PHP_INT_MAX => 'max'];
  try { $m[] = 'overflow'; } catch (Error $e) { $m['err'] = get_class($e); }
  $x = [&$a]; $x[0] = 'changed';
  $w = [1.5 => 'f', true => 't', null => 'n'];
  $e = []; $single = [s('only')]; $keyed = [s('k') => s('v')];
  return [$l[0], $l[3], count($l), $m, $a, $w, $single, $keyed, count($e)];
}
for ($i = 0; $i < 2; $i++) echo json_encode(t($i)), "|\n";
?>
--EXPECTF--

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line %d

Deprecated: Using null as an array offset is deprecated, use an empty string instead in %s on line %d
~d0 ["x","t",7,{"k":"x","5":"five","7":7,"dyn":[1],"nested":{"x":0},"0":"r","9223372036854775807":"max","err":"Error"},"changed",{"1":"t","":"n"},["only"],{"k":"v"},0]|

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line %d

Deprecated: Using null as an array offset is deprecated, use an empty string instead in %s on line %d
~d1 ["x","t",7,{"k":"x","5":"five","7":7,"dyn":[1],"nested":{"x":1},"1":"r","9223372036854775807":"max","err":"Error"},"changed",{"1":"t","":"n"},["only"],{"k":"v"},0]|
