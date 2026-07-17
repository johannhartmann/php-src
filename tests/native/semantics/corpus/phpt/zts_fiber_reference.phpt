--TEST--
W01 semantic corpus: ZTS fiber frame preserves a referenced value
--SKIPIF--
<?php
if (!PHP_ZTS) {
    die('skip ZTS build required');
}
?>
--FILE--
<?php
$value = 'main';
$reference =& $value;
$fiber = new Fiber(function () use (&$reference): void {
    $reference = 'fiber';
    Fiber::suspend();
    $reference = 'resumed';
});

$fiber->start();
echo "suspended:$value\n";
$fiber->resume();
echo "returned:$value\n";
?>
--EXPECT--
suspended:fiber
returned:resumed
