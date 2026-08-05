--TEST--
Native calls inherited internal methods with the current object receiver
--SKIPIF--
<?php
if (!extension_loaded('native_mir_test')) {
    die('skip native_mir_test extension not available');
}
?>
--FILE--
<?php
class NativeInheritedArrayIterator extends ArrayIterator
{
    public function first(): mixed
    {
        $this->rewind();
        return $this->current();
    }
}

$iterator = new NativeInheritedArrayIterator(['first', 'second']);
var_dump($iterator->first());
?>
--EXPECT--
string(5) "first"
