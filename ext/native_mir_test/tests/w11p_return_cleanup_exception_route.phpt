--TEST--
Native return cleanup routes iterator destructor exceptions outside exited catch regions
--FILE--
<?php
final class ReturnCleanupIterator
{
    public $first = 1;
    public $second = 2;

    public function __destruct()
    {
        throw new Exception('cleanup');
    }
}

function return_cleanup_inner()
{
    foreach (new ReturnCleanupIterator() as $value) {
        try {
            return $value;
        } catch (Exception) {
            echo "inner\n";
        } finally {
            echo "finally\n";
        }
    }
    return 0;
}

try {
    return_cleanup_inner();
} catch (Exception) {
    echo "outer\n";
}
?>
--EXPECT--
finally
outer
