--TEST--
Native direct internal calls with by-reference arguments
--DESCRIPTION--
Internal calls taking SEND_REF arguments (preg_match, array_pop, next,
array_unshift, sort, str_replace's count) push the frame, send their
arguments and call the handler in one step: a CV, undefined ones included,
or an IS_INDIRECT dimension, property or static property becomes a
reference; exceptions still reach finally and catch.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class Q { public array $items = [3, 1, 2]; public $map = ['k' => [5, 4]]; public static $st = [9, 8]; public int $n = 0; }
function run($i) {
    $o = [];
    $o[] = preg_match('/(\w)(\d)/', "x$i", $m) . json_encode($m);         // undefined CV becomes a reference
    $q = new Q;
    $o[] = array_pop($q->items) . json_encode($q->items);                  // typed property, indirect
    $o[] = array_pop($q->map['k']) . json_encode($q->map);                 // dimension of a property
    $o[] = array_pop(Q::$st) . count(Q::$st);
    $list = ['a', 'b', 'c'];
    $o[] = next($list) . next($list) . current($list) . (next($list) === false ? 'F' : 'T');
    $o[] = array_unshift($list, $i, 'z') . implode('', $list);
    $copy = $list; sort($copy); $o[] = implode('', $copy) . '/' . implode('', $list);
    $r = [2, 1]; $alias = &$r; sort($alias); $o[] = implode('', $r);
    $o[] = str_replace('a', 'b', 'banana', $count) . $count;
    try {
        try { $x = 5; sort($x); } finally { $o[] = 'finally'; }
    } catch (TypeError $e) { $o[] = get_class($e); }
    try { array_pop($q->n); } catch (Error $e) { $o[] = substr($e->getMessage(), 0, 30); }
    return implode(' ', $o);
}
for ($i = 0; $i < 3; $i++) { echo run($i), "\n"; }
?>
--EXPECT--
1["x0","x","0"] 2[3,1] 4{"k":[5]} 81 bccF 50zabc 0abcz/0zabc 12 bbnbnb3 finally TypeError array_pop(): Argument #1 ($arr
1["x1","x","1"] 2[3,1] 4{"k":[5]} 90 bccF 51zabc 1abcz/1zabc 12 bbnbnb3 finally TypeError array_pop(): Argument #1 ($arr
1["x2","x","2"] 2[3,1] 4{"k":[5]} 0 bccF 52zabc 2abcz/2zabc 12 bbnbnb3 finally TypeError array_pop(): Argument #1 ($arr
