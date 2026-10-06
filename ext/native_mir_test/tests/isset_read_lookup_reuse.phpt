--TEST--
isset() or array_key_exists() followed by a read or write fetch of the same element reuses the lookup
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function f($a, $k) { if (isset($a[$k])) { return $a[$k]; } return 'none'; }
function lit($a) { if (isset($a['x'])) { return $a['x']; } return 'none'; }
function idx($a) { if (isset($a[3])) { return $a[3]; } return 'none'; }
function tern($a, $k) { return isset($a[$k]) ? $a[$k] : 'd'; }
function neg($a, $k) { if (!isset($a[$k])) { return 'neg'; } return $a[$k]; }
function loop($rows, $k) { $out = []; foreach ($rows as $r) { if (isset($r[$k])) { $out[] = $r[$k]; } else { $out[] = '-'; } } return implode(',', $out); }
function global_ref($k) { global $G; if (isset($G[$k])) { return $G[$k]; } return 'g-none'; }
class AA implements ArrayAccess {
    public function offsetExists($o): bool { echo "exists($o) "; return $o !== 'no'; }
    public function offsetGet($o): mixed { echo "get($o) "; return "v:$o"; }
    public function offsetSet($o, $v): void {}
    public function offsetUnset($o): void {}
}
$G = ['a' => 1, 'b' => null, '5' => 'five'];
$ref = 'r'; $arr = ['k' => &$ref, 'n' => null, 'z' => 0, '7' => 'seven', 7 => 'int7', 'x' => 'X', 3 => 'three'];
foreach (['k', 'n', 'z', '7', 7, 'missing', 'x', 3, '3', 1.5, true] as $k) {
    echo var_export($k, true), ': ', var_export(f($arr, $k), true), ' ', var_export(tern($arr, $k), true), ' ', var_export(neg($arr, $k), true), "\n";
}
echo lit($arr), lit([]), lit(['x' => null]), idx($arr), idx([3 => [1]])[0] ?? '', "\n";
echo f('hello', 1), f('hello', 9), f(null, 'a'), f(42, 'a'), "\n";
echo f(new AA, 'yes'), "\n", f(new AA, 'no'), "\n";
echo loop([['k' => 1], [], ['k' => null], ['k' => 'v'], 'str', ['k' => [1, 2]]], 'k'), "\n";
echo global_ref('a'), global_ref('b'), global_ref(5), global_ref('zz'), "\n";
$big = []; for ($i = 0; $i < 100; $i++) { $big["k$i"] = $i; }
$s = 0; foreach ($big as $kk => $_) { $s += f($big, $kk); } echo $s, "\n";
$r2 = ['a' => [1]]; $x = &$r2['a']; echo json_encode(f($r2, 'a')), "\n";
function g($a, $path) { foreach ($path as $p) { if (!array_key_exists($p, $a)) return 'miss'; $a = $a[$p]; } return $a; }
function ake($a, $k) { if (array_key_exists($k, $a)) { return $a[$k]; } return 'none'; }
function akel($a) { if (array_key_exists('x', $a)) { return $a['x']; } return 'none'; }
function aken($a) { if (array_key_exists('5', $a)) { return $a[5]; } return 'none'; }
$ref = 'r'; $arr = ['k' => &$ref, 'n' => null, 'x' => 'X', 5 => 'five', 'a' => ['b' => ['c' => 3]]];
foreach (['k', 'n', 'x', 5, '5', 'missing'] as $k) { echo var_export(ake($arr, $k), true), ' '; }
echo "\n", var_export(akel($arr), true), var_export(akel([]), true), var_export(akel(['x' => null]), true), aken($arr), "\n";
echo g($arr, ['a', 'b', 'c']), g($arr, ['a', 'zz']), json_encode(g($arr, ['a'])), var_export(g($arr, ['n']), true), "\n";
$s = 0; for ($i = 0; $i < 1000; $i++) { $s += g(['p' => ['q' => $i]], ['p', 'q']); } echo $s, "\n";
function count_hook($k) { global $hooks; if (!isset($hooks[$k])) { $hooks[$k] = 1; } else { ++$hooks[$k]; } }
function inc_local($a, $k) { if (isset($a[$k])) { ++$a[$k]; } return $a; }
function inc_shared($a, $k) { $b = $a; if (isset($a[$k])) { ++$a[$k]; } return [$a[$k] ?? null, $b[$k] ?? null]; }
function app($a, $k) { if (isset($a[$k])) { $a[$k][] = 'x'; } return $a; }
function ake_w($a, $k) { if (array_key_exists($k, $a)) { $a[$k] .= '!'; } return $a; }
for ($i = 0; $i < 5; $i++) { count_hook('init'); count_hook('wp'); } count_hook('init');
var_dump($hooks);
var_dump(inc_local(['n' => 1, 'z' => null], 'n'), inc_local(['n' => 1], 'm'));
var_dump(inc_shared(['n' => 1], 'n'));
var_dump(app(['l' => [1]], 'l'), ake_w(['s' => 'a', 'n' => null], 's'), ake_w(['n' => null], 'n'));
$r = 5; $ra = ['r' => &$r]; var_dump(inc_local($ra, 'r'), $r);
$g = ['q' => 1]; $alias = &$g; function inc_g($k) { global $g; if (isset($g[$k])) { ++$g[$k]; } } inc_g('q'); var_dump($g, $alias);
--EXPECTF--
'k': 'r' 'r' 'r'
'n': 'none' 'd' 'neg'
'z': 0 0 0
'7': 'int7' 'int7' 'int7'
7: 'int7' 'int7' 'int7'
'missing': 'none' 'd' 'neg'
'x': 'X' 'X' 'X'
3: 'three' 'three' 'three'
'3': 'three' 'three' 'three'
1.5: 
Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 2
'none' 
Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 5
'd' 
Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 6
'neg'
true: 'none' 'd' 'neg'
Xnonenonethree1
enonenonenone
exists(yes) get(yes) v:yes
exists(no) none

Warning: Array to string conversion in %s on line 7
1,-,-,v,-,Array
1g-nonefiveg-none
4950
[1]
'r' NULL 'X' 'five' 'five' 'none' 
'X''none'NULLfive
3miss{"b":{"c":3}}NULL
499500
array(2) {
  ["init"]=>
  int(6)
  ["wp"]=>
  int(5)
}
array(2) {
  ["n"]=>
  int(2)
  ["z"]=>
  NULL
}
array(1) {
  ["n"]=>
  int(1)
}
array(2) {
  [0]=>
  int(2)
  [1]=>
  int(1)
}
array(1) {
  ["l"]=>
  array(2) {
    [0]=>
    int(1)
    [1]=>
    string(1) "x"
  }
}
array(2) {
  ["s"]=>
  string(2) "a!"
  ["n"]=>
  NULL
}
array(1) {
  ["n"]=>
  string(1) "!"
}
array(1) {
  ["r"]=>
  &int(6)
}
int(6)
array(1) {
  ["q"]=>
  int(2)
}
array(1) {
  ["q"]=>
  int(2)
}
