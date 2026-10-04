--TEST--
Native foreach overwriting shared loop values and freeing shared holders inline
--DESCRIPTION--
FE_FETCH_R overwrites a loop variable whose old value another owner keeps
alive and that needs no new GC root, as the previous element does; a
reference, a last owner and a possible GC root keep the helper. FE_FREE of
an array holder with another owner drops one reference inline. Arrays of
arrays, strings and objects, keys, break, object iterators, generators,
temporaries and collectable cycles.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class Node { public $next; public $v; function __construct($v) { $this->v = $v; } }
function strs($n) { $a = []; for ($i = 0; $i < $n; $i++) { $a["k$i"] = str_repeat('s', $i + 1); } return $a; }
function loops($round)
{
    $out = [];
    $nested = ['a' => [1, 2], 'b' => ['x' => 'y'], 'c' => [], 'd' => [3]];
    foreach ($nested as $k => $v) { $out[] = "$k:" . count($v); }
    foreach ($nested as $v) { $out[] = json_encode($v); }
    $s = strs(4);
    foreach ($s as $k => $str) { $out[] = "$k=$str"; }
    $held = null;
    foreach ($s as $str) { $held = $str; }
    $out[] = $held;
    $objs = ['p' => new Node(1), 'q' => new Node(2)];
    foreach ($objs as $o) { $out[] = $o->v; }
    $v = str_repeat('own', 2);           // loop var starts as last owner
    foreach (['x' => 1, 'y' => 2] as $v) { $out[] = $v; }
    $ref = 'r'; $alias = &$ref;
    foreach (['m' => 'n'] as $alias) { }
    $out[] = $ref;
    $mixed = ['i' => 1, 's' => 'two', 'a' => [3], 'f' => 4.5, 'n' => null, 5 => 'five'];
    foreach ($mixed as $k => $m) { $out[] = gettype($k) . ':' . json_encode($m); }
    $copy = $nested;
    foreach ($copy as $k => $v) { $copy[$k] = 'replaced'; }
    $out[] = json_encode($copy);
    $out[] = json_encode($nested);
    // cycles through loop variables stay collectable
    for ($i = 0; $i < 50; $i++) { $a = new Node($i); $b = new Node($i); $a->next = $b; $b->next = $a; $list = ['a' => [$a], 'b' => [$b]]; foreach ($list as $e) { } }
    $out[] = gc_collect_cycles() >= 0;
    foreach ($nested as $k => $v) { if ($k === 'b') { break; } }
    $out[] = $k;
    foreach (new ArrayIterator(['it' => 'er']) as $k => $v) { $out[] = "$k$v"; }
    foreach ((function () { yield 'g' => 'en'; })() as $k => $v) { $out[] = "$k$v"; }
    foreach (strs(2) as $k => $v) { $out[] = $v; }   // temporary, last owner
    return $out;
}
for ($r = 0; $r < 3; $r++) { echo json_encode(loops($r)), "\n"; }
echo memory_get_usage() < 4000000 ? "mem ok" : "mem high", "\n";
?>
--EXPECT--
["a:2","b:1","c:0","d:1","[1,2]","{\"x\":\"y\"}","[]","[3]","k0=s","k1=ss","k2=sss","k3=ssss","ssss",1,2,1,2,"n","string:1","string:\"two\"","string:[3]","string:4.5","string:null","integer:\"five\"","{\"a\":\"replaced\",\"b\":\"replaced\",\"c\":\"replaced\",\"d\":\"replaced\"}","{\"a\":[1,2],\"b\":{\"x\":\"y\"},\"c\":[],\"d\":[3]}",true,"b","iter","gen","s","ss"]
["a:2","b:1","c:0","d:1","[1,2]","{\"x\":\"y\"}","[]","[3]","k0=s","k1=ss","k2=sss","k3=ssss","ssss",1,2,1,2,"n","string:1","string:\"two\"","string:[3]","string:4.5","string:null","integer:\"five\"","{\"a\":\"replaced\",\"b\":\"replaced\",\"c\":\"replaced\",\"d\":\"replaced\"}","{\"a\":[1,2],\"b\":{\"x\":\"y\"},\"c\":[],\"d\":[3]}",true,"b","iter","gen","s","ss"]
["a:2","b:1","c:0","d:1","[1,2]","{\"x\":\"y\"}","[]","[3]","k0=s","k1=ss","k2=sss","k3=ssss","ssss",1,2,1,2,"n","string:1","string:\"two\"","string:[3]","string:4.5","string:null","integer:\"five\"","{\"a\":\"replaced\",\"b\":\"replaced\",\"c\":\"replaced\",\"d\":\"replaced\"}","{\"a\":[1,2],\"b\":{\"x\":\"y\"},\"c\":[],\"d\":[3]}",true,"b","iter","gen","s","ss"]
mem ok
