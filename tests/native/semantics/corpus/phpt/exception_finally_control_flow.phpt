--TEST--
W01 semantic corpus: return and throw preserve finally cleanup order
--FILE--
<?php
function returnThroughFinally(array &$events): string
{
    try {
        $events[] = 'return:try';
        return 'return:value';
    } finally {
        $events[] = 'return:finally';
    }
}

function throwThroughFinally(array &$events): void
{
    try {
        $events[] = 'throw:try';
        throw new RuntimeException('throw:value');
    } finally {
        $events[] = 'throw:finally';
    }
}

$events = [];
$events[] = returnThroughFinally($events);
try {
    throwThroughFinally($events);
} catch (RuntimeException $exception) {
    $events[] = 'catch:' . $exception->getMessage();
}
echo implode("\n", $events), "\n";
?>
--EXPECT--
return:try
return:finally
return:value
throw:try
throw:finally
catch:throw:value
