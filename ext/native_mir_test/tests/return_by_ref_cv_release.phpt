--TEST--
Native leave releases a CV a by-reference return made a reference
--DESCRIPTION--
RETURN_BY_REF turns its CV into a reference with no SSA definition; the
fast-call entry and the direct activation must still release that CV.
--EXTENSIONS--
native_mir_test
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function &f() {
    $x = "foo";
    return ($a);
}
function &g($n) {
    $s = str_repeat('s', $n);
    return $s;
}
$fn = 'f';
$gn = 'g';
for ($i = 0; $i < 8; $i++) {
    $h =& $fn();
    $k =& $gn(3);
}
var_dump($h, $k);
for ($i = 0; $i < 8; $i++) {
    $h2 =& f();
}
var_dump($h2);
?>
--EXPECT--
NULL
string(3) "sss"
NULL
