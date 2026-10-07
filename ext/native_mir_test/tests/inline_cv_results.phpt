--TEST--
count(), is_*() and array_key_exists() write a CV result inline when its old value needs no release
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class C implements Countable { function count(): int { return 7; } }
function probe($v, $k) {
    $r = [];
    $n = count(is_array($v) || $v instanceof Countable ? $v : []); $r[] = $n;
    $a = [str_repeat('x', 2)];
    $a = is_array($v); $r[] = var_export($a, true);
    $s = str_repeat('y', 3);
    $s = is_string($v); $r[] = var_export($s, true);
    $e = [1];
    $e = is_array($v) && array_key_exists($k, $v); $r[] = var_export($e, true);
    $h = str_repeat('h', 2);
    $h = array_key_exists('a', ['a' => 1, 'b' => null]); $r[] = var_export($h, true);
    $c = new stdClass;
    $c = count([1, 2, $v]); $r[] = $c;
    $ref = [1, 2, 3];
    $alias = &$ref;
    $m = count($alias); $r[] = $m;
    return implode(',', $r);
}
foreach ([[1, 2], ['a' => 1], 'str', 5, null, new C] as $v) {
    echo probe($v, 'a'), "\n";
}
--EXPECTF--
2,true,false,false,true,3,3
1,true,false,true,true,3,3
0,false,true,false,true,3,3
0,false,false,false,true,3,3
0,false,false,false,true,3,3
7,false,false,false,true,3,3
