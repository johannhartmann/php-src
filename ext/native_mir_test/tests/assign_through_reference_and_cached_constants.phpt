--TEST--
Native assignments through references and cached constants
--DESCRIPTION--
An assignment to a CV that holds a reference writes the referent inline unless
the reference has typed property sources. FETCH_CONSTANT copies a cached
constant with a non-refcounted value from its runtime cache slot.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class D { function __construct(public $n) {} function __destruct() { echo "d{$this->n} "; } }
class P { public int $i = 0; public ?string $s = null; }
function glb($k) { global $G; $G = $k * 2; $t = $G; $G = "s$k"; $u = $G; return [$t, $u, $G]; }
$G = 1; var_dump(glb(3), $G);
function stat_($k) { static $s = 0; $s = $s + $k; $x = ($s = $s * 2); return [$s, $x]; }
var_dump(stat_(1), stat_(2));
function alias() { $a = "abc" . mt_rand(0, 0); $b = &$a; $b = 5; $c = $a; $b = str_repeat("z", 3); return [$a, $b, $c]; }
var_dump(alias());
function typed() { $p = new P; $r = &$p->i; try { $r = "12"; } catch (Throwable $e) { echo get_class($e), "\n"; } var_dump($p->i); try { $r = "x"; } catch (Throwable $e) { echo get_class($e), ": ", $e->getMessage(), "\n"; } $q = &$p->s; $q = 5; var_dump($p); }
typed();
function arr() { $a = [1, 2]; $r = &$a; $r = 3; $x = $a; $a = [4]; return [$x, $r]; }
var_dump(arr());
function dtor() { $o = new D(1); $r = &$o; $r = 2; echo "after\n"; return $o; }
var_dump(dtor());
function loopref($n) { $sum = 0; $r = &$sum; for ($i = 0; $i < $n; $i++) { $r = $r + $i; } return $sum; }
var_dump(loopref(100));
define("IM", 139968); define("IA", 3877); define("IC", 29573);
function gen_random ($n) { global $LAST; return( ($n * ($LAST = ($LAST * IA + IC) % IM)) / IM ); }
function fill($N) { global $LAST; $LAST = 42; for ($i=1; $i<=$N; $i++) { $ary[$i] = gen_random(1); } return $ary; }

$LAST = 42; var_dump(array_sum(fill(200)), $LAST, IM, IA + IC);

?>

--EXPECT--
array(3) {
  [0]=>
  int(6)
  [1]=>
  string(2) "s3"
  [2]=>
  string(2) "s3"
}
string(2) "s3"
array(2) {
  [0]=>
  int(2)
  [1]=>
  int(2)
}
array(2) {
  [0]=>
  int(8)
  [1]=>
  int(8)
}
array(3) {
  [0]=>
  string(3) "zzz"
  [1]=>
  string(3) "zzz"
  [2]=>
  int(5)
}
int(12)
TypeError: Cannot assign string to reference held by property P::$i of type int
object(P)#1 (2) {
  ["i"]=>
  &int(12)
  ["s"]=>
  &string(1) "5"
}
array(2) {
  [0]=>
  int(3)
  [1]=>
  array(1) {
    [0]=>
    int(4)
  }
}
d1 after
int(2)
int(4950)
float(105.18372770919066)
int(119554)
int(139968)
int(33450)
