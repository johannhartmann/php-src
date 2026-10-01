--TEST--
Positional by-value arguments copied without operand decoding
--DESCRIPTION--
SEND_VAL and SEND_VAR(_EX) of CVs, temporaries and literals to by-value
parameters of internal and dynamically called functions take the copy fast
path; referenced CVs are dereferenced, undefined CVs still warn, and
by-reference and prefer-reference parameters keep the general path.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function user3($a, $b, $c) { return "$a|$b|$c"; }
function run($s, $n) {
    $r = &$s;
    $parts = [str_repeat($s, 2), substr($r . 'xyz', 1, $n), strtoupper('lit'), max($n, 3, $n * 2)];
    $parts[] = call_user_func_array('user3', [$s, $n + 1, 'c']);
    $parts[] = call_user_func('user3', $r, strlen($s), [1][0]);
    $parts[] = preg_match('/(b)/', $s . 'b', $m) . $m[1];
    $arr = [3, 1, 2];
    sort($arr);
    $parts[] = implode(',', $arr);
    $data = [3, 1]; $keys = ['c', 'a'];
    array_multisort($data, $keys);
    $parts[] = implode(',', $keys);
    return implode(' ', $parts);
}
for ($i = 0; $i < 3; $i++) { echo run('ab', $i + 1), "\n"; }
echo strlen($undefined ?? ''), ' ', @strlen($undefined), "\n";
echo strlen($undefined2), "\n";
?>
--EXPECTF--
abab b LIT 3 ab|2|c ab|2|1 1b 1,2,3 a,c
abab bx LIT 4 ab|3|c ab|2|1 1b 1,2,3 a,c
abab bxy LIT 6 ab|4|c ab|2|1 1b 1,2,3 a,c
0 0

Warning: Undefined variable $undefined2 in %s on line 19

Deprecated: strlen(): Passing null to parameter #1 ($string) of type string is deprecated in %s on line 19
0
