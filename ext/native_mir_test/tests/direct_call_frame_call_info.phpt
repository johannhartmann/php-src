--TEST--
Native direct calls publish their frame's call info
--DESCRIPTION--
A direct call's frame carries its call flags and argument count; a call without arguments sets both with one store. func_num_args(), func_get_args(), backtraces and method, static and closure calls read them like the VM.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function none() { return func_num_args(); }
function none_args() { return func_get_args(); }
function none_trace() { $t = debug_backtrace(); return [$t[0]['function'], count($t[0]['args'] ?? [])]; }
function one($a) { return func_num_args() . ":" . $a; }
function extra() { return func_num_args() . ":" . implode(",", func_get_args()); }
function typed(): int { return 7; }
class K {
    private int $v = 3;
    function m() { return func_num_args() + $this->v; }
    function chain() { return $this->m() + $this->m(); }
    static function s() { return func_num_args(); }
    function closure() { return (function () { return $this->v * func_num_args(); })(); }
}
function caller() {
    $k = new K;
    $r = [];
    $r[] = none();
    $r[] = none_args();
    $r[] = none_trace();
    $r[] = one(5);
    $r[] = extra();
    $r[] = extra(1, 2, 3);
    $r[] = typed();
    $r[] = $k->m();
    $r[] = $k->chain();
    $r[] = K::s();
    $r[] = $k->closure();
    for ($i = 0; $i < 3; $i++) {
        $r[] = none() + typed();
    }
    return $r;
}
var_export(caller());
echo "\n";
try {
    (function () { throw new Exception("x"); })();
} catch (Exception $e) {
    echo count($e->getTrace()), "\n";
}
?>
--EXPECT--
array (
  0 => 0,
  1 => 
  array (
  ),
  2 => 
  array (
    0 => 'none_trace',
    1 => 0,
  ),
  3 => '1:5',
  4 => '0:',
  5 => '3:1,2,3',
  6 => 7,
  7 => 3,
  8 => 6,
  9 => 0,
  10 => 0,
  11 => 7,
  12 => 7,
  13 => 7,
)
1
