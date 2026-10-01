--TEST--
Native direct array reads and concatenation into CV results
--DESCRIPTION--
FETCH_DIM_R and CONCAT/FAST_CONCAT whose result the optimizer placed in a
CV take the direct helpers; the CV, which holds no counted value, is
overwritten as the VM overwrites it, and the general path re-encodes the
CV result.
--EXTENSIONS--
opcache
native_mir_test
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function run($a, $s, $n) {
    $out = [];
    for ($i = 0; $i < $n; $i++) {
        $v = $a['k'];
        $w = $a[$i];
        $x = $s . $i;
        $y = $s . 'lit';
        $z = $i . $s;
        $m = @$a['missing'];
        $f = $a['f'] . 1.5;
        $out[] = "$v|$w|$x|$y|$z|" . var_export($m, true) . "|$f";
    }
    $q = $a['nokey'];
    $out[] = var_export($q, true);
    return implode("\n", $out);
}
echo run(['k' => 'kv', 0 => 'zero', 1 => 1, 2 => 2.5, 'f' => 2], 'S', 3), "\n";
?>
--EXPECTF--

Warning: Undefined array key "nokey" in %s on line 14
kv|zero|S0|Slit|0S|NULL|21.5
kv|1|S1|Slit|1S|NULL|21.5
kv|2.5|S2|Slit|2S|NULL|21.5
NULL
