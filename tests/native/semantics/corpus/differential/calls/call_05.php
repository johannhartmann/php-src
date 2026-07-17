<?php
require __DIR__ . '/repeated_calls.inc';
$state = 0;
for ($call = 1; $call <= 5; $call++) {
    $result = semanticRepeatedCall($state, $call);
}
echo "call-05:$result:state=$state\n";
