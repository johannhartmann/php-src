<?php
require __DIR__ . '/repeated_calls.inc';
$state = 0;
for ($call = 1; $call <= 9; $call++) {
    $result = semanticRepeatedCall($state, $call);
}
echo "call-09:$result:state=$state\n";
