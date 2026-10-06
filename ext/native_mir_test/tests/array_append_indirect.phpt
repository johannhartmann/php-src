--TEST--
Appends to an array a write fetch addresses
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class Box { public array $arr = []; public $loose; }
function nested(array $keys, $value) {
    $a = [];
    foreach ($keys as $i => $k) {
        $a[$k][] = $value;
        $a[$k][] = [$i, $k];
    }
    return $a;
}
function shared_inner() {
    $a = ['x' => [1]];
    $copies = [];
    for ($i = 0; $i < 3; $i++) {
        $copy = $a['x'];
        $a['x'][] = $i;
        $copies[] = $copy;
    }
    return [$a, $copies];
}
function referenced_inner() {
    $a = ['x' => [1]];
    $r = &$a['x'];
    for ($i = 0; $i < 3; $i++) { $a['x'][] = $i; }
    $r[] = 'via ref';
    return $a;
}
function hash_inner() {
    $a = ['x' => ['k' => 1]];
    for ($i = 0; $i < 3; $i++) { $a['x'][] = $i; }
    return $a;
}
function properties(Box $b) {
    for ($i = 0; $i < 3; $i++) {
        $b->arr[] = $i;
        $b->loose[] = [$i];
    }
    return $b;
}
function self_value() {
    $a = ['x' => [1]];
    $v = [2, 3];
    for ($i = 0; $i < 2; $i++) { $a['x'][] = $v; $v[] = $i; }
    return [$a, $v];
}
for ($r = 0; $r < 3; $r++) {
    echo json_encode(nested(['a', 'b', 'a', 7, 7], 'v')), "\n";
    echo json_encode(shared_inner()), "\n";
    echo json_encode(referenced_inner()), "\n";
    echo json_encode(hash_inner()), "\n";
    echo json_encode(properties(new Box)), "\n";
    echo json_encode(self_value()), "\n";
}
?>
--EXPECT--
{"a":["v",[0,"a"],"v",[2,"a"]],"b":["v",[1,"b"]],"7":["v",[3,7],"v",[4,7]]}
[{"x":[1,0,1,2]},[[1],[1,0],[1,0,1]]]
{"x":[1,0,1,2,"via ref"]}
{"x":{"k":1,"0":0,"1":1,"2":2}}
{"arr":[0,1,2],"loose":[[0],[1],[2]]}
[{"x":[1,[2,3],[2,3,0]]},[2,3,0,1]]
{"a":["v",[0,"a"],"v",[2,"a"]],"b":["v",[1,"b"]],"7":["v",[3,7],"v",[4,7]]}
[{"x":[1,0,1,2]},[[1],[1,0],[1,0,1]]]
{"x":[1,0,1,2,"via ref"]}
{"x":{"k":1,"0":0,"1":1,"2":2}}
{"arr":[0,1,2],"loose":[[0],[1],[2]]}
[{"x":[1,[2,3],[2,3,0]]},[2,3,0,1]]
{"a":["v",[0,"a"],"v",[2,"a"]],"b":["v",[1,"b"]],"7":["v",[3,7],"v",[4,7]]}
[{"x":[1,0,1,2]},[[1],[1,0],[1,0,1]]]
{"x":[1,0,1,2,"via ref"]}
{"x":{"k":1,"0":0,"1":1,"2":2}}
{"arr":[0,1,2],"loose":[[0],[1],[2]]}
[{"x":[1,[2,3],[2,3,0]]},[2,3,0,1]]
