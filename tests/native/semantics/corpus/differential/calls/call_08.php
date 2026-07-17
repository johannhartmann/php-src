<?php
require __DIR__ . '/repeated_calls.inc';
$state = 0;
for ($call = 1; $call <= 8; $call++) {
    $result = semanticRepeatedCall($state, $call);
}
echo "call-08:$result:state=$state\n";
