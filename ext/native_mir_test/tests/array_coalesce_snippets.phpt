--TEST--
Native ?? reads of array elements through EncodeGen snippets
--DESCRIPTION--
FETCH_DIM_IS of CV, temporary and literal containers under literal and CV
keys: existing, missing and null elements, references, numeric-looking and
runtime built keys, null, false, number and undefined containers, which read
as null without a diagnostic, string offsets, ArrayAccess objects, nested
reads through temporaries, $GLOBALS and colliding keys.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class Box { public $list = ['a' => 1]; public function __get($n) { return "magic $n"; } }
class AA implements ArrayAccess {
    function offsetExists($o): bool { echo "exists($o) "; return $o === 'x'; }
    function offsetGet($o): mixed { echo "get($o) "; return "v$o"; }
    function offsetSet($o, $v): void {} function offsetUnset($o): void {}
}
function temp() { return ['t' => ['u' => 'deep'], 0 => null]; }
function f($map, $key, $null, $str, $obj, $num)
{
    $alias = 5; $map['ref'] = &$alias; $runtime = implode('', ['na', 'me']);
    $undef_key = null;
    return [
        $map['name'] ?? 'd1', $map['missing'] ?? 'd2', $map[$key] ?? 'd3', $map[$runtime] ?? 'd4',
        $map['nil'] ?? 'd5', $map['ref'] ?? 'd6', $map[10] ?? 'd7', $map['10'] ?? 'd8', $map[-1] ?? 'd9',
        $null['x'] ?? 'n1', $null[$key] ?? 'n2', $num['x'] ?? 'n3', false['x'] ?? 'n4',
        $str[0] ?? 's1', $str['x'] ?? 's2', $str[10] ?? 's3',
        $obj['x'] ?? 'o1', $obj['y'] ?? 'o2',
        temp()['t']['u'] ?? 't1', temp()['t']['v'] ?? 't2', temp()[0] ?? 't3', temp()['zz']['q'] ?? 't4',
        $map['deep']['a']['b'] ?? 'x1', $map['deep']['a']['c'] ?? 'x2', $map['deep']['z']['c'] ?? 'x3',
        $undefined['k'] ?? 'u1', $map[$undef_var ?? 'name'] ?? 'u2',
        (new Box)->list['a'] ?? 'b1', (new Box)->list['q'] ?? 'b2',
        $GLOBALS['gv'] ?? 'g1', $GLOBALS['nogv'] ?? 'g2',
    ];
}
$gv = 'global';
$map = ['name' => 'native', 'nil' => null, 10 => 'ten', -1 => 'minus', 'deep' => ['a' => ['b' => 'B']]];
for ($i = 0; $i < 3; $i++) {
    echo json_encode(f($map, $i ? 'name' : 'zz', null, 'str', new AA, 42)), "\n";
}
$many = [];
for ($i = 0; $i < 100; $i++) { $many["k$i"] = $i; }
function g($many) { $s = 0; foreach (['k0', 'k50', 'k99', 'k100', 'zz'] as $k) { $s += $many[$k] ?? 1000; } return $s; }
echo g($many), "\n";
?>
--EXPECT--
exists(x) get(x) exists(y) ["native","d2","d3","native","d5",5,"ten","ten","minus","n1","n2","n3","n4","s","s2","s3","vx","o2","deep","t2","t3","t4","B","x2","x3","u1","native",1,"b2","global","g2"]
exists(x) get(x) exists(y) ["native","d2","native","native","d5",5,"ten","ten","minus","n1","n2","n3","n4","s","s2","s3","vx","o2","deep","t2","t3","t4","B","x2","x3","u1","native",1,"b2","global","g2"]
exists(x) get(x) exists(y) ["native","d2","native","native","d5",5,"ten","ten","minus","n1","n2","n3","n4","s","s2","s3","vx","o2","deep","t2","t3","t4","B","x2","x3","u1","native",1,"b2","global","g2"]
2149
