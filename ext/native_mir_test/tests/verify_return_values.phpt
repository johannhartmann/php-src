--TEST--
Native: return-type checks of plain values, references and coercions
--DESCRIPTION--
Return values the declaration names pass at once; references, coercions,
constants and errors take the full check.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
namespace NativeReturnValues;

class Leaf {}

function asInt($v): int { return $v; }
function viaRef(array &$a): int { $r = &$a[0]; return $r; }
function leafRef(array &$a): Leaf { $r = &$a[0]; return $r; }
function &byRef(array &$a): int { return $a[0]; }
function constant(): string { return 'c'; }
function nullable($v): ?int { return $v; }
function union($v): int|string { return $v; }
function tmp($v): string { return $v . '!'; }

$a = [7];
$l = [new Leaf];
for ($i = 0; $i < 3; $i++) {
    var_dump(asInt(1), asInt('2'), asInt(3.0), viaRef($a), get_class(leafRef($l)));
    $r = &byRef($a);
    $r++;
    var_dump($a[0], constant(), nullable(null), union('u'), union(4), tmp('t'));
}
foreach (['x', 1.5, null, []] as $bad) {
    try {
        asInt($bad);
    } catch (\TypeError $e) {
        echo $e->getMessage(), "\n";
    }
}
?>
--EXPECTF--
int(1)
int(2)
int(3)
int(7)
string(23) "NativeReturnValues\Leaf"
int(8)
string(1) "c"
NULL
string(1) "u"
int(4)
string(2) "t!"
int(1)
int(2)
int(3)
int(8)
string(23) "NativeReturnValues\Leaf"
int(9)
string(1) "c"
NULL
string(1) "u"
int(4)
string(2) "t!"
int(1)
int(2)
int(3)
int(9)
string(23) "NativeReturnValues\Leaf"
int(10)
string(1) "c"
NULL
string(1) "u"
int(4)
string(2) "t!"
NativeReturnValues\asInt(): Return value must be of type int, string returned

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 6
NativeReturnValues\asInt(): Return value must be of type int, null returned
NativeReturnValues\asInt(): Return value must be of type int, array returned
