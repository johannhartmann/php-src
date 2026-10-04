--TEST--
Native call entry receives defaults and checks typed parameters
--EXTENSIONS--
native_mir_test
--FILE--
<?php
function defaults($a, $b = 7, $c = 3000000000, $d = 1.5, $e = true, $f = null, $g = 'str', $h = [1, 'x' => 2]) {
    return json_encode([func_num_args(), $a, $b, $c, $d, $e, $f, $g, $h, func_get_args()]);
}
function typed(int $i, string $s = 'd', ?float $f = null, bool $b = false) {
    return var_export([$i, $s, $f, $b], true);
}
function trace($x = 'default') { return count(debug_backtrace()[0]['args']) . ':' . $x; }
function thrower($n = 1) { if ($n > 0) throw new RuntimeException("n=$n"); return $n; }
function recurse($n, $acc = 0) { return $n === 0 ? $acc : recurse($n - 1, $acc + $n); }
function noargs() { return func_num_args(); }
function cvs($a, $b = 2) { $c = $a + $b; $d = [$c]; $e = "$c"; return $e . count($d); }
class K { public function m($a, $b = 'mb') { return "$a$b"; } public static function s(int $x = 4) { return $x * 2; } }
$k = new K;
for ($round = 0; $round < 3; $round++) {
    echo defaults(1), "\n", defaults(1, 2), "\n", defaults(1, 2, 3, 4, 5, 6, 7, 8), "\n";
    echo defaults(a: 9, g: 'named'), "\n";
    echo typed(5), ' ', typed(6, 'x', 2.5, true), ' ', typed('7', 8, 3), "\n";
    try { echo typed('nan'), "\n"; } catch (TypeError $e) { echo get_class($e), ': ', $e->getMessage(), "\n"; }
    try { echo defaults(), "\n"; } catch (ArgumentCountError $e) { echo get_class($e), "\n"; }
    echo trace(), ' ', trace('given'), "\n";
    try { thrower(); } catch (RuntimeException $e) { echo $e->getMessage(), ' ', $e->getTrace()[0]['function'], "\n"; }
    echo thrower(0), ' ', recurse(10), ' ', noargs(), ' ', noargs(1, 2), ' ', cvs(1), ' ', cvs(1, 5), "\n";
    echo $k->m('a'), ' ', $k->m('a', 'b'), ' ', K::s(), ' ', K::s(5), "\n";
    try { echo K::s('x'), "\n"; } catch (TypeError $e) { echo 'TypeError', "\n"; }
}
--EXPECTF--
[1,1,7,3000000000,1.5,true,null,"str",{"0":1,"x":2},[1]]
[2,1,2,3000000000,1.5,true,null,"str",{"0":1,"x":2},[1,2]]
[8,1,2,3,4,5,6,7,8,[1,2,3,4,5,6,7,8]]
[7,9,7,3000000000,1.5,true,null,"named",{"0":1,"x":2},[9,7,3000000000,1.5,true,null,"named"]]
array (
  0 => 5,
  1 => 'd',
  2 => NULL,
  3 => false,
) array (
  0 => 6,
  1 => 'x',
  2 => 2.5,
  3 => true,
) array (
  0 => 7,
  1 => '8',
  2 => 3.0,
  3 => false,
)
TypeError: typed(): Argument #1 ($i) must be of type int, string given, called in %s on line 19
ArgumentCountError
0:default 1:given
n=1 thrower
0 55 0 2 31 61
amb ab 8 10
TypeError
[1,1,7,3000000000,1.5,true,null,"str",{"0":1,"x":2},[1]]
[2,1,2,3000000000,1.5,true,null,"str",{"0":1,"x":2},[1,2]]
[8,1,2,3,4,5,6,7,8,[1,2,3,4,5,6,7,8]]
[7,9,7,3000000000,1.5,true,null,"named",{"0":1,"x":2},[9,7,3000000000,1.5,true,null,"named"]]
array (
  0 => 5,
  1 => 'd',
  2 => NULL,
  3 => false,
) array (
  0 => 6,
  1 => 'x',
  2 => 2.5,
  3 => true,
) array (
  0 => 7,
  1 => '8',
  2 => 3.0,
  3 => false,
)
TypeError: typed(): Argument #1 ($i) must be of type int, string given, called in %s on line 19
ArgumentCountError
0:default 1:given
n=1 thrower
0 55 0 2 31 61
amb ab 8 10
TypeError
[1,1,7,3000000000,1.5,true,null,"str",{"0":1,"x":2},[1]]
[2,1,2,3000000000,1.5,true,null,"str",{"0":1,"x":2},[1,2]]
[8,1,2,3,4,5,6,7,8,[1,2,3,4,5,6,7,8]]
[7,9,7,3000000000,1.5,true,null,"named",{"0":1,"x":2},[9,7,3000000000,1.5,true,null,"named"]]
array (
  0 => 5,
  1 => 'd',
  2 => NULL,
  3 => false,
) array (
  0 => 6,
  1 => 'x',
  2 => 2.5,
  3 => true,
) array (
  0 => 7,
  1 => '8',
  2 => 3.0,
  3 => false,
)
TypeError: typed(): Argument #1 ($i) must be of type int, string given, called in %s on line 19
ArgumentCountError
0:default 1:given
n=1 thrower
0 55 0 2 31 61
amb ab 8 10
TypeError
