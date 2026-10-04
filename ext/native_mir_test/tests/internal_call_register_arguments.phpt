--TEST--
Native register-held arguments of internal calls stored directly
--DESCRIPTION--
A register-held by-value argument of a bound internal function, an exact
integer, float, boolean or null or a temporary, is stored into the pushed
frame's argument slot directly, as SEND_VAL stores it, instead of through
the argument helpers.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function args(int $n, float $f, bool $b, ?string $s) {
    $o = [];
    $o[] = str_repeat('ab', $n + 1);
    $o[] = round($f * 3, 2);
    $o[] = json_encode(array_fill(0, $n, $b));
    $o[] = json_encode(array_pad([], $n % 3, !$b));
    $o[] = var_export(is_null($s ?? null), true);
    $o[] = str_pad((string) ($n * 2), $n + 3, '0', STR_PAD_LEFT);
    $o[] = substr(strtoupper("xyz$n"), $n % 2, 2);
    $o[] = implode(',', range($n, $n + 2));
    $o[] = max($n, $f, $n + 0.5);
    $o[] = sprintf('%05.1f|%d|%s', $f, $n, $b ? 'T' : 'F');
    $o[] = number_format($f * 1000, $n % 3);
    return implode(' ', $o);
}
for ($i = 0; $i < 4; $i++) { echo args($i, $i / 3, $i % 2 === 0, $i ? "s$i" : null), "\n"; }
?>
--EXPECT--
ab 0 [] [] true 000 XY 0,1,2 0.5 000.0|0|T 0
abab 1 [false] [true] false 0002 YZ 1,2,3 1.5 000.3|1|F 333.3
ababab 2 [true,true] [false,false] false 00004 XY 2,3,4 2.5 000.7|2|T 666.67
abababab 3 [false,false,false] [] false 000006 YZ 3,4,5 3.5 001.0|3|F 1,000
