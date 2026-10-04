--TEST--
if/while on arrays and plain objects without decoding the branch
--DESCRIPTION--
JMPZ/JMPNZ of an array decide by its element count and of an object with
the standard cast by true; shared temporaries are released, while a
temporary whose release would run a destructor and objects with their
own cast keep the general path.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class D { function __destruct() { echo "~D;"; } }
function arr($n) { return $n ? array_fill(0, $n, 1) : []; }
function fresh() { return [new D]; }
function run($k) {
    $out = [];
    $a = arr($k); $o = new stdClass; $e = [];
    if ($a) { $out[] = 'a'; }
    if (!$e) { $out[] = 'e'; }
    if ($o) { $out[] = 'o'; }
    if (arr($k)) { $out[] = 't'; }
    if (fresh()) { $out[] = 'f'; }
    $i = 0; while (arr(2 - $i)) { $i++; } $out[] = $i;
    if (new ArrayObject([])) { $out[] = 'ao'; }
    return implode(',', $out);
}
for ($k = 0; $k < 3; $k++) { echo run($k), "\n"; }
?>
--EXPECT--
~D;e,o,f,2,ao
~D;a,e,o,t,f,2,ao
~D;a,e,o,t,f,2,ao
