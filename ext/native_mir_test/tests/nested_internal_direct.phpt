--TEST--
Native nested internal calls transfer their arguments at DO
--DESCRIPTION--
A group of nested calls to internal functions bound without scope, such as
array_merge($a, array_keys($b)), runs as direct calls that transfer their
arguments at DO instead of VM-ordered INIT/SEND/DO fragments. A pending
temporary argument of the outer call is released when the inner call
throws; user callbacks inside the inner call keep their semantics.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function names($node, $media) {
    $names = array_keys($node['elements'] ?? []);
    foreach (array_keys($media) as $bp) {
        $names = array_merge($names, array_keys($node[$bp]['elements'] ?? []));
    }
    return implode(',', array_unique($names));
}
function pending($s) {
    try {
        return array_merge([str_repeat($s, 2) . '!'], json_decode('{', true, 512, JSON_THROW_ON_ERROR));
    } catch (JsonException $e) {
        return 'caught ' . $e->getMessage();
    }
}
function callback($list) {
    $seen = [];
    $out = implode('|', array_map(function ($v) use (&$seen) { $seen[] = $v; return strtoupper($v); }, array_filter($list)));
    return $out . ' ' . count($seen);
}
function by_ref_inner($a) {
    $x = [1, 2];
    return implode(',', array_merge($x, array_slice($a, 1)));
}
$node = ['elements' => ['a' => 1, 'b' => 2], 'sm' => ['elements' => ['c' => 1]], 'md' => ['elements' => ['a' => 1, 'd' => 2]]];
for ($i = 0; $i < 3; $i++) {
    echo names($node, ['sm' => 1, 'md' => 2, 'lg' => 3]), ' ', pending("p$i"), ' ', callback(['x', '', "y$i"]), ' ', by_ref_inner([7, 8, $i]), "\n";
}
?>
--EXPECT--
a,b,c,d caught Syntax error near location 1:2 X|Y0 2 1,2,8,0
a,b,c,d caught Syntax error near location 1:2 X|Y1 2 1,2,8,1
a,b,c,d caught Syntax error near location 1:2 X|Y2 2 1,2,8,2
