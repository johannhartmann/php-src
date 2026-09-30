--TEST--
Native ++/-- of integer CVs and write-fetched elements
--DESCRIPTION--
++ and -- of an integer CV, or of the integer element a write fetch's VAR
points to, update it without decoding the operation. Overflow, strings,
floats, typed properties, references, null and undefined variables keep
stock semantics.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class C { public $n = 0; public int $t = PHP_INT_MAX - 1; public float $f = 1.5; public ?int $m = null; function bump() { $this->n++; ++$this->n; $x = $this->n++; return [$x, $this->n]; } }
function t() {
  $a = ['k' => 1, 'max' => PHP_INT_MAX, 's' => '5', 'f' => 1.5]; $c = new C;
  $a['k']++; ++$a['k']; $p = $a['k']--; $q = --$a['k']; $a['max']++; $a['s']++; $a['f']++; $a['new'] ??= 0; $a['new']++;
  $r = $c->bump(); $c->t++; try { $c->t++; } catch (Error $e) { $r[] = get_class($e); } $c->f++; $c->m++;
  $ref = &$a['k']; $a['k']++;
  $i = 5; $i++; ++$i; $j = $i--; $u = @$undefined++;
  return [$a, $p, $q, $r, $c->t, $c->f, $c->m, $i, $j, $u];
}
echo json_encode(t()), "\n";
?>
--EXPECT--
[{"k":2,"max":9.223372036854776e+18,"s":6,"f":2.5,"new":1},3,1,[2,3,"TypeError"],9223372036854775807,2.5,1,6,7,null]
