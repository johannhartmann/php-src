--TEST--
Tier 2 copies specialized by argument feedback release the arguments their failed guards hand to the tier-1 code
--EXTENSIONS--
opcache
--ENV--
ZEND_NATIVE_TIER2_THRESHOLD=2
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
$dir = sys_get_temp_dir() . '/tier2_arg_guard_' . getmypid();
@mkdir($dir);
$lib = $dir . '/lib.php';
file_put_contents($lib, <<<'LIB'
<?php
class Square {}
function take($v) {}
function take_two($i, $v) { return $i; }
LIB);
require $lib;
$a = new Square;
$b = new Square;
$values = [0, 1, $a, 'x' . mt_rand(1, 1), $b, [mt_rand(1, 1)]];
for ($round = 0; $round < 3; $round++) {
    foreach ($values as $j => $v) {
        take($v);
        take_two($j, $v);
    }
}
unset($v, $values);
debug_zval_dump($a);
debug_zval_dump($b);
unlink($lib); rmdir($dir);
?>
--EXPECT--
object(Square)#1 (0) refcount(2){
}
object(Square)#2 (0) refcount(2){
}
