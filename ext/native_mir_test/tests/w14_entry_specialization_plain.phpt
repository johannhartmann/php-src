--TEST--
Native entry specialization for untyped integer parameters without OPcache
--DESCRIPTION--
A function whose untyped parameters feed integer arithmetic or comparisons
in loops, or in recursion with exact integer arguments, also compiles a
variant with those parameters declared int. The entry checks the arguments
once and runs the variant on the same frame; other argument types, extra
arguments, backtraces, exceptions and integer overflow behave as in the VM.
--INI--
opcache.enable=0
opcache.enable_cli=0
opcache.file_update_protection=0
--FILE--
<?php
function loops($n) { $x = 0; for ($a = 0; $a < $n; $a++) for ($b = 0; $b < $n; $b++) $x += $a * $b; return $x; }
function fibv($n) { return $n < 2 ? 1 : fibv($n - 2) + fibv($n - 1); }
function mixed_params($n, $label, &$out) { for ($i = 0; $i < $n; $i++) { $out[] = "$label$i"; } return count($out); }
function args_seen($n) { $s = 0; for ($i = 0; $i < $n; $i++) { $s += $i; } return [$s, func_get_args(), func_num_args()]; }
function trace($n) { for ($i = 0; $i < $n; $i++) {} return debug_backtrace()[0]['args']; }
function thrower($n) { for ($i = 0; $i < $n; $i++) { if ($i === 2) throw new RuntimeException("at $i"); } return $n; }
function overflow($n) { $x = PHP_INT_MAX - 2; for ($i = 0; $i < $n; $i++) { $x++; } return $x; }
for ($r = 0; $r < 2; $r++) {
    var_dump(loops(4), loops(4.5), loops("3"), loops(true), fibv(12), fibv(5.0));
    $o = []; var_dump(mixed_params(3, "x", $o), $o);
    var_dump(args_seen(3, "extra"), trace(2), overflow(5), overflow(1.5));
    try { thrower(5); } catch (RuntimeException $e) { echo $e->getMessage(), " ", $e->getTrace()[0]['function'], "\n"; }
    var_dump(loops(null), loops(PHP_INT_MAX > 0 ? 2 : 0));
}
?>
--EXPECT--
int(36)
int(100)
int(9)
int(0)
int(233)
int(8)
int(3)
array(3) {
  [0]=>
  string(2) "x0"
  [1]=>
  string(2) "x1"
  [2]=>
  string(2) "x2"
}
array(3) {
  [0]=>
  int(3)
  [1]=>
  array(2) {
    [0]=>
    int(3)
    [1]=>
    string(5) "extra"
  }
  [2]=>
  int(2)
}
array(1) {
  [0]=>
  int(2)
}
float(9.223372036854776E+18)
int(9223372036854775807)
at 2 thrower
int(0)
int(1)
int(36)
int(100)
int(9)
int(0)
int(233)
int(8)
int(3)
array(3) {
  [0]=>
  string(2) "x0"
  [1]=>
  string(2) "x1"
  [2]=>
  string(2) "x2"
}
array(3) {
  [0]=>
  int(3)
  [1]=>
  array(2) {
    [0]=>
    int(3)
    [1]=>
    string(5) "extra"
  }
  [2]=>
  int(2)
}
array(1) {
  [0]=>
  int(2)
}
float(9.223372036854776E+18)
int(9223372036854775807)
at 2 thrower
int(0)
int(1)
