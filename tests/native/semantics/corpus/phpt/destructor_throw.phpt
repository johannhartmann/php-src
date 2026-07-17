--TEST--
W01 semantic corpus: destructor throw remains catchable and ordered
--FILE--
<?php
class ThrowingDestructor
{
    public function __destruct()
    {
        echo "destructor:throw\n";
        throw new RuntimeException('from-destructor');
    }
}

$value = new ThrowingDestructor();
echo "before-unset\n";
try {
    unset($value);
} catch (RuntimeException $exception) {
    echo 'caught:', $exception->getMessage(), "\n";
}
echo "after-catch\n";
?>
--EXPECT--
before-unset
destructor:throw
caught:from-destructor
after-catch
