--TEST--
Native object-property conditions remain authoritative across repeated entries
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
class PropertyConditionalIterator extends ArrayIterator
{
    public $useKey = 1;

    public function choose(): mixed
    {
        return $this->useKey ? $this->key() : $this->current();
    }
}

$iterator = new PropertyConditionalIterator(['a', 'b', 'c']);
foreach ($iterator as $_) {
    var_dump($iterator->choose());
}
?>
--EXPECT--
int(0)
int(1)
int(2)
