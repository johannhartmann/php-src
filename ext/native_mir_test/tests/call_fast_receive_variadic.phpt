--TEST--
Native fast call receive of variadic, extra and defaulted arguments
--DESCRIPTION--
Fast call sites of variadic targets, of targets sent more arguments than
they declare, and dynamic call sites receive their parameters without the
generic frame preparation: extra arguments move behind the CVs as the VM
moves them, a variadic parameter collects them (or is an empty array), and
missing parameters take their literal defaults. func_get_args(),
backtraces, references and refcounted extra arguments keep stock semantics.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function filters($name, $value, ...$args) {
    $local = strtoupper($name);
    return $local . '=' . $value . '[' . implode(',', $args) . ']' . count($args);
}
function extra($a, $b = 'd') { return $a . $b . '|' . implode(',', func_get_args()) . '|' . func_num_args(); }
function trace_args($a, ...$rest) { return implode(',', array_map('json_encode', debug_backtrace()[0]['args'])); }
function by_ref_rest(&$x, ...$rest) { $x .= '!'; return count($rest); }
function counted(...$all) { $all[0][] = 'x'; return count($all[0]) . ':' . count($all); }
function hook($v, $extra = 'e') { return "$v$extra"; }
function dispatch($callbacks, $args) {
    $out = [];
    foreach ($callbacks as $cb) {
        $out[] = call_user_func_array($cb, $args);
    }
    return implode(' ', $out);
}
$arr = ['a', 'b'];
for ($round = 0; $round < 3; $round++) {
    echo filters('h', 1), ' ', filters('h', 2, 'x'), ' ', filters('h', 3, 'x', str_repeat('y', $round + 1)), "\n";
    echo extra('a'), ' ', extra('a', 'b'), ' ', extra('a', 'b', 'c', str_repeat('z', 2)), "\n";
    echo trace_args(1, [2], 'three'), "\n";
    $s = 'r';
    echo by_ref_rest($s, 1, 2), $s, "\n";
    echo counted($arr, 1), ' ', count($arr), "\n";
    echo dispatch(['hook', fn($v, $w = 'w') => "$v$w", 'trace_args'], ['v']), "\n";
    echo dispatch(['hook', 'extra'], ['v', 'x', 'y']), "\n";
}
?>
--EXPECT--
H=1[]0 H=2[x]1 H=3[x,y]2
ad|a|1 ab|a,b|2 ab|a,b,c,zz|4
1,[2],"three"
2r!
3:2 2
ve vw "v"
vx vx|v,x,y|3
H=1[]0 H=2[x]1 H=3[x,yy]2
ad|a|1 ab|a,b|2 ab|a,b,c,zz|4
1,[2],"three"
2r!
3:2 2
ve vw "v"
vx vx|v,x,y|3
H=1[]0 H=2[x]1 H=3[x,yyy]2
ad|a|1 ab|a,b|2 ab|a,b,c,zz|4
1,[2],"three"
2r!
3:2 2
ve vw "v"
vx vx|v,x,y|3
