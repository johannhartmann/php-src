--TEST--
Native call entry collects variadic parameters
--EXTENSIONS--
native_mir_test
--FILE--
<?php
function filters($hook, $value, ...$args) { return "$hook:" . json_encode($value) . ':' . json_encode($args) . ':' . func_num_args() . ':' . count(func_get_args()); }
function only(...$all) { $all[] = 'local'; return json_encode($all); }
function defaulted($a, $b = 'B', ...$rest) { return "$a|$b|" . json_encode($rest); }
function traced($a, ...$r) { return json_encode(debug_backtrace()[0]['args']); }
function byref_rest($a, &...$r) { foreach ($r as &$x) { $x .= '!'; } return count($r); }
function typed_rest($a, int ...$r) { return array_sum($r) + $a; }
function thrower(...$a) { throw new Exception('v' . count($a)); }
function many($a, $b, $c, ...$d) { $local = $a . $b . $c; return $local . count($d); }
$o = new stdClass; $o->p = 1;
$arr = [1, 2];
for ($round = 0; $round < 3; $round++) {
    echo filters('h', 1), "\n", filters('h', [1], 'x'), "\n", filters('h', $o, $arr, "s$round", 3.5, null), "\n";
    echo only(), ' ', only(1), ' ', only($arr, $o), "\n";
    echo defaulted(1), ' ', defaulted(1, 2), ' ', defaulted(1, 2, 3, 4), "\n";
    echo traced(1, 2, 3), "\n";
    $s = 'a'; $t = 'b'; echo byref_rest(0, $s, $t), " $s $t\n";
    echo typed_rest(1, 2, 3), ' ';
    try { echo typed_rest(1, 'x'); } catch (TypeError $e) { echo 'TypeError'; }
    echo "\n";
    try { thrower(1, 2); } catch (Exception $e) { echo $e->getMessage(), "\n"; }
    echo many(1, 2, 3), many(1, 2, 3, 4, 5), "\n";
    echo call_user_func_array('filters', ['dyn', $round, 'a', 'b']), ' ', call_user_func('only', 1, 2), "\n";
    echo filters('u', ...$arr), ' ', filters(...['n', 'm', 'o']), "\n";
    try { echo filters('x'); } catch (ArgumentCountError $e) { echo "ArgumentCountError\n"; }
}
echo json_encode($o), "\n";
--EXPECTF--
h:1:[]:2:2
h:[1]:["x"]:3:3
h:{"p":1}:[[1,2],"s0",3.5,null]:6:6
["local"] [1,"local"] [[1,2],{"p":1},"local"]
1|B|[] 1|2|[] 1|2|[3,4]
[1,2,3]
2 a! b!
6 TypeError
v2
12301232
dyn:0:["a","b"]:4:4 [1,2,"local"]
u:1:[2]:3:3 n:"m":["o"]:3:3
ArgumentCountError
h:1:[]:2:2
h:[1]:["x"]:3:3
h:{"p":1}:[[1,2],"s1",3.5,null]:6:6
["local"] [1,"local"] [[1,2],{"p":1},"local"]
1|B|[] 1|2|[] 1|2|[3,4]
[1,2,3]
2 a! b!
6 TypeError
v2
12301232
dyn:1:["a","b"]:4:4 [1,2,"local"]
u:1:[2]:3:3 n:"m":["o"]:3:3
ArgumentCountError
h:1:[]:2:2
h:[1]:["x"]:3:3
h:{"p":1}:[[1,2],"s2",3.5,null]:6:6
["local"] [1,"local"] [[1,2],{"p":1},"local"]
1|B|[] 1|2|[] 1|2|[3,4]
[1,2,3]
2 a! b!
6 TypeError
v2
12301232
dyn:2:["a","b"]:4:4 [1,2,"local"]
u:1:[2]:3:3 n:"m":["o"]:3:3
ArgumentCountError
{"p":1}
