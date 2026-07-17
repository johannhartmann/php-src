<?php
require __DIR__ . '/repeated_calls.inc';
$state = 0;
for ($call = 1; $call <= 7; $call++) {
    $result = semanticRepeatedCall($state, $call);
}
echo "call-07:$result:state=$state\n";
