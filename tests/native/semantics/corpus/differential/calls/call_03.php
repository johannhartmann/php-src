<?php
require __DIR__ . '/repeated_calls.inc';
$state = 0;
for ($call = 1; $call <= 3; $call++) {
    $result = semanticRepeatedCall($state, $call);
}
echo "call-03:$result:state=$state\n";
