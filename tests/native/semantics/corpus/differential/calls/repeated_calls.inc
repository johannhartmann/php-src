<?php
function semanticRepeatedCall(int &$state, int $call): string
{
    $original = ['call' => $call, 'state' => $state];
    $copy = $original;
    $copy['state'] += $call;
    $state = $copy['state'];
    return $original['state'] . ':' . $copy['state'];
}
