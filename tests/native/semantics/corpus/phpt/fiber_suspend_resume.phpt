--TEST--
W01 semantic corpus: fiber suspend and resume preserve transfer state
--FILE--
<?php
$fiber = new Fiber(function (string $start): string {
    echo "fiber:start:$start\n";
    $resume = Fiber::suspend('suspended-value');
    echo "fiber:resume:$resume\n";
    return 'fiber-return';
});

$suspended = $fiber->start('argument');
echo 'main:start:', $suspended, "\n";
echo 'main:suspended:', $fiber->isSuspended() ? 'yes' : 'no', "\n";
$fiber->resume('resume-value');
echo 'main:return:', $fiber->getReturn(), "\n";
?>
--EXPECT--
fiber:start:argument
main:start:suspended-value
main:suspended:yes
fiber:resume:resume-value
main:return:fiber-return
