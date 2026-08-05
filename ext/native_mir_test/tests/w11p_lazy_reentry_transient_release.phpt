--TEST--
Native lazy reentry releases compiler-only transient state after publication
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
class W11LazyReentryStringable
{
    public function __toString(): string
    {
        return 'native';
    }
}

$start = memory_get_usage();
for ($index = 0; $index < 1000; $index++) {
    $digest = md5(new W11LazyReentryStringable());
}
$growth = memory_get_usage() - $start;

var_dump($digest === md5('native'));
var_dump($growth < 16 * 1024);
?>
--EXPECT--
bool(true)
bool(true)
