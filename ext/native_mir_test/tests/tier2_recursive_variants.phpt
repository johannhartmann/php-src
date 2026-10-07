--TEST--
Tier 2 copies keep their integer variants: recursive calls stay in the copies, with argument feedback
--EXTENSIONS--
opcache
--ENV--
ZEND_NATIVE_TIER2_THRESHOLD=3
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
$dir = sys_get_temp_dir() . '/tier2_recursion_' . getmypid();
@mkdir($dir);
$lib = $dir . '/lib.php';
file_put_contents($lib, <<<'LIB'
<?php
function fib($n) { return $n < 2 ? $n : fib($n - 1) + fib($n - 2); }
function ack($m, $n) {
    if ($m == 0) return $n + 1;
    if ($n == 0) return ack($m - 1, 1);
    return ack($m - 1, ack($m, ($n - 1)));
}
function mixed_rec($n) { return $n <= 0 ? 0 : 1 + mixed_rec($n - 1); }
LIB);
require $lib;
$f = 'fib';
for ($i = 0; $i < 8; $i++) {
    echo fib(12 + $i % 3), ' ', $f(10), ' ', ack(2, $i), ' ', mixed_rec($i % 2 ? $i : (float) $i), ' ', mixed_rec("$i"), "\n";
}
unlink($lib); rmdir($dir);
--EXPECT--
144 55 3 0 0
233 55 5 1 1
377 55 7 2 2
144 55 9 3 3
233 55 11 4 4
377 55 13 5 5
144 55 15 6 6
233 55 17 7 7
