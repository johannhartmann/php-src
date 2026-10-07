--TEST--
=== and !== write their bool into a CV inline, releasing nothing; a counted old value or an undefined operand takes the helper
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function cmp($x, $y) {
    $r = [];
    $b = $x === 'abc'; $r[] = var_export($b, true);
    $b = 'abc' !== $x; $r[] = var_export($b, true);
    $b = null === $x; $r[] = var_export($b, true);
    $b = $x === $y; $r[] = var_export($b, true);
    $c = [1, 2, 3];
    $c = $x === 'abc';
    $r[] = var_export($c, true);
    $d = str_repeat('z', 3);
    $d = $y !== $x;
    $r[] = var_export($d, true);
    return implode(',', $r);
}
foreach (['abc', 'abd', null, 0, '0', [1], 1.5, true] as $x) {
    foreach (['abc', null, 0] as $y) {
        echo cmp($x, $y), "\n";
    }
}
function undef_cmp() { $b = $nope === 'x'; return var_export($b, true); }
echo undef_cmp(), "\n";
--EXPECTF--
true,false,false,true,true,false
true,false,false,false,true,true
true,false,false,false,true,true
false,true,false,false,false,true
false,true,false,false,false,true
false,true,false,false,false,true
false,true,true,false,false,true
false,true,true,true,false,false
false,true,true,false,false,true
false,true,false,false,false,true
false,true,false,false,false,true
false,true,false,true,false,false
false,true,false,false,false,true
false,true,false,false,false,true
false,true,false,false,false,true
false,true,false,false,false,true
false,true,false,false,false,true
false,true,false,false,false,true
false,true,false,false,false,true
false,true,false,false,false,true
false,true,false,false,false,true
false,true,false,false,false,true
false,true,false,false,false,true
false,true,false,false,false,true

Warning: Undefined variable $nope in %s on line 21
false
