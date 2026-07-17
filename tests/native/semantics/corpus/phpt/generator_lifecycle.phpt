--TEST--
W01 semantic corpus: generator yield-from, throw, and close preserve state
--FILE--
<?php
function generatorSequence(array &$events): Generator
{
    try {
        yield 'one';
        yield from ['two', 'three'];
        try {
            yield 'throw-point';
        } catch (RuntimeException $exception) {
            $events[] = 'caught:' . $exception->getMessage();
        }
        yield 'after-throw';
    } finally {
        $events[] = 'closed:finally';
    }
}

$events = [];
$generator = generatorSequence($events);
echo 'yield:', $generator->current(), "\n";
for ($index = 0; $index < 3; $index++) {
    $generator->next();
    echo 'yield:', $generator->current(), "\n";
}
echo 'throw-result:', $generator->throw(new RuntimeException('injected')), "\n";
unset($generator);
echo implode("\n", $events), "\n";
?>
--EXPECT--
yield:one
yield:two
yield:three
yield:throw-point
throw-result:after-throw
caught:injected
closed:finally
