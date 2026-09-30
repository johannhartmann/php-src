--TEST--
Native write fetches of array elements through EncodeGen snippets
--DESCRIPTION--
FETCH_DIM_W, RW and UNSET of existing elements of unshared arrays return the
element's INDIRECT inline: nested writes through CV and VAR containers,
compound assignments and increments, references into elements, foreach by
reference, static, immutable and $GLOBALS arrays and property arrays.
Shared arrays, missing keys, appends, autovivification and string offsets
take the helper.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class Holder {
    public $p = ['a' => ['b' => 1]];
    public function bump($k) { $this->p['a'][$k] = ($this->p['a'][$k] ?? 0) + 1; $this->p[$k]['x'] = 1; return $this->p; }
}
function writes($key, $round)
{
    $a = ['x' => ['y' => 1, 'z' => [1, 2]], 5 => [0 => 'p'], 'list' => [1, 2]];
    $copy = $a;                         // shared outer array: first write separates
    $a['x']['y'] = 2;
    $a['x']['z'][1] = 'two';
    $a['x']['new'] = 'n';               // missing inner key
    $a['n1']['n2'] = 'auto';            // missing outer key: autovivification
    $a[5][0] .= 'q';                    // FETCH_DIM_RW
    $a['x']['y'] += 10;
    $a['x']['y']++;
    $a[$key]['y'] = "key $key";
    $a['list'][] = 3;                   // append through a write fetch
    unset($a['x']['z'][0]);             // FETCH_DIM_UNSET
    unset($a['nope']['deeper']);
    $r = &$a['x']['ref'];               // reference into an element
    $r = 'via ref';
    $inner = $a['x'];                   // shares the inner array
    $a['x']['y'] = 'after share';       // separation of the inner array
    $b = [[[0]]];
    for ($i = 0; $i < 3; $i++) { $b[0][0][$i] = $i * $round; }
    $imm = ['const' => ['k' => 1]];
    $imm['const']['k'] = 2;             // immutable literal array
    static $st = ['s' => ['c' => 0]];
    $st['s']['c']++;
    $GLOBALS['gw']['k']['j'] = $round;
    $str = 'abc';
    try { $str[0][0] = 'x'; } catch (Error $e) { $out[] = get_class($e); }
    $null = null;
    $null['a']['b'] = 1;
    $h = new Holder;
    $out[] = $h->bump($key);
    return [$a, $copy['x'], $inner, $b, $imm, $st, $null, $out];
}
$gw = ['k' => ['j' => -1]];
for ($r = 0; $r < 3; $r++) {
    echo json_encode(writes($r ? 'x' : 'fresh', $r)), "\n";
}
echo json_encode($gw), "\n";
function refs() {
    $arr = ['a' => ['b' => [1, 2, 3]]];
    foreach ($arr['a']['b'] as &$v) { $v *= 2; }
    unset($v);
    $alias = &$arr['a'];
    $alias['b'][] = 7;
    $arr['a']['b'][0] = 'first';
    return json_encode([$arr, $alias]);
}
echo refs(), "\n";
?>
--EXPECT--
[{"x":{"y":"after share","z":{"1":"two"},"new":"n","ref":"via ref"},"5":["pq"],"list":[1,2,3],"n1":{"n2":"auto"},"fresh":{"y":"key fresh"}},{"y":1,"z":[1,2]},{"y":13,"z":{"1":"two"},"new":"n","ref":"via ref"},[[[0,0,0]]],{"const":{"k":2}},{"s":{"c":1}},{"a":{"b":1}},["Error",{"a":{"b":1,"fresh":1},"fresh":{"x":1}}]]
[{"x":{"y":"after share","z":{"1":"two"},"new":"n","ref":"via ref"},"5":["pq"],"list":[1,2,3],"n1":{"n2":"auto"}},{"y":1,"z":[1,2]},{"y":"key x","z":{"1":"two"},"new":"n","ref":"via ref"},[[[0,1,2]]],{"const":{"k":2}},{"s":{"c":2}},{"a":{"b":1}},["Error",{"a":{"b":1,"x":1},"x":{"x":1}}]]
[{"x":{"y":"after share","z":{"1":"two"},"new":"n","ref":"via ref"},"5":["pq"],"list":[1,2,3],"n1":{"n2":"auto"}},{"y":1,"z":[1,2]},{"y":"key x","z":{"1":"two"},"new":"n","ref":"via ref"},[[[0,2,4]]],{"const":{"k":2}},{"s":{"c":3}},{"a":{"b":1}},["Error",{"a":{"b":1,"x":1},"x":{"x":1}}]]
{"k":{"j":2}}
[{"a":{"b":["first",4,6,7]}},{"b":["first",4,6,7]}]
