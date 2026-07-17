--TEST--
W01 semantic corpus: destructor reenters PHP before cleanup completes
--FILE--
<?php
function reenteredFunction(): void
{
    echo "reentered\n";
}

class ReenteringDestructor
{
    public function __destruct()
    {
        echo "destructor:begin\n";
        reenteredFunction();
        echo "destructor:end\n";
    }
}

$value = new ReenteringDestructor();
echo "before-unset\n";
unset($value);
echo "after-unset\n";
?>
--EXPECT--
before-unset
destructor:begin
reentered
destructor:end
after-unset
