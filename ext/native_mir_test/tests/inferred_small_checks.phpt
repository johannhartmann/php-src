--TEST--
Native type checks, coalesce, foreach, array literals, ropes and string keys follow inferred types
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function checks($v) {
    $r = '';
    if (is_string($v)) { $r .= 's'; }
    if (is_array($v)) { $r .= 'a'; }
    if ($v === null) { $r .= 'n'; }
    $alias = &$v;
    if (is_int($alias)) { $r .= 'i'; }
    if (is_string(@$missing)) { $r .= 'm'; }
    return $r;
}
function coalesce(array $a, string $k) {
    $text = str_repeat('x', 3);
    $list = [$text, $text];
    $value = $a[$k] ?? $list;
    $copy = $text ?? 'none';
    return count($value) . $copy;
}
function loop(array $items) {
    $out = [];
    foreach ($items as $key => $item) {
        $out[] = $item;
    }
    return $out;
}
function literal(string $s) {
    $t = $s . '!';
    $arr = [$t, $s, [$t]];
    $rope = "pre {$t} mid {$s} post";
    return [$arr, $rope];
}
function keys(array $map, string $key) {
    $hit = isset($map[$key]) ? $map[$key] : 'miss';
    $map[$key] = 'set';
    return $hit . $map[$key] . (isset($map['k1']) ? 'y' : 'n');
}
foreach (['str', [1], null, 7] as $v) { echo checks($v), "|"; }
echo "\n", coalesce(['a' => [1, 2, 3]], 'a'), coalesce([], 'b'), "\n";
$objs = [new stdClass, str_repeat('y', 2), [3]];
echo json_encode(loop($objs)), "\n";
echo json_encode(literal(str_repeat('z', 2))), "\n";
echo keys(['k1' => 'v1', '5' => 'five'], 'k1'), keys([], str_repeat('k', 2)), keys(['5' => 'n'], '5'), "\n";
?>
--EXPECT--
s|a|n|i|
3xxx2xxx
[{},"yy",[3]]
[["zz!","zz",["zz!"]],"pre zz! mid zz post"]
v1setymisssetnnsetn
