--TEST--
Native count() of arrays through an EncodeGen snippet
--DESCRIPTION--
count() and sizeof() of array CVs, references and temporaries inline, in
branches and loop conditions; Countable objects, strings, undefined
variables and COUNT_RECURSIVE take the helper.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class Cnt implements Countable { function count(): int { return 7; } }
function arr() { return [1, 2, 3]; }
function counts($a, $r, $o, $s)
{
    $alias = $a; $ref = &$alias; $shared = $a; $shared[] = 9;
    $out = [count($a), count($ref), count(arr()), count([1, 2]), count($shared), count($o), sizeof($a)];
    if (count($a) > 2) { $out[] = 'big'; }
    $n = 0; for ($i = 0; $i < count($a); $i++) { $n += $a[$i]; } $out[] = $n;
    try { $out[] = count($s); } catch (TypeError $e) { $out[] = $e->getMessage(); }
    try { $out[] = count($undefined); } catch (TypeError $e) { $out[] = get_class($e); }
    $out[] = count(array_filter($a));
    $out[] = count($r, COUNT_RECURSIVE);
    return $out;
}
for ($i = 0; $i < 3; $i++) {
    echo json_encode(counts([1, 2, 3], [[1, 2], [3]], new Cnt, 'str')), "\n";
}
?>
--EXPECTF--

Warning: Undefined variable $undefined in %s on line %d
[3,3,3,2,4,7,3,"big",6,"count(): Argument #1 ($value) must be of type Countable|array, string given","TypeError",3,5]

Warning: Undefined variable $undefined in %s on line %d
[3,3,3,2,4,7,3,"big",6,"count(): Argument #1 ($value) must be of type Countable|array, string given","TypeError",3,5]

Warning: Undefined variable $undefined in %s on line %d
[3,3,3,2,4,7,3,"big",6,"count(): Argument #1 ($value) must be of type Countable|array, string given","TypeError",3,5]
