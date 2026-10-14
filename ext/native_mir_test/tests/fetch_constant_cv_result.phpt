--TEST--
Native constant fetch into a CV result
--DESCRIPTION--
The optimizer contracts "$cv = CONSTANT" into a FETCH_CONSTANT whose result
is the CV. Both the cached fetch and the helper must accept that result.
--EXTENSIONS--
native_mir_test
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
define(strtoupper('myc'), 'uncategorized');
function r($a) {
    $x = null;
    $area = MYC;
    $t = $a['t'] ?? strlen('q');
    return $area . $t;
}
for ($i = 0; $i < 50; $i++) { $o = r(['t' => 1]); }
var_dump($o);
?>
--EXPECT--
string(14) "uncategorized1"
