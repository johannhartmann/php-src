<?php
require __DIR__ . '/repeated_calls.inc';
$state = 0;
for ($call = 1; $call <= 2; $call++) {
    $result = semanticRepeatedCall($state, $call);
}
echo "call-02:$result:state=$state\n";
