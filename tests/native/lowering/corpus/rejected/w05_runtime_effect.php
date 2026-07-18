<?php

function w03_rejected_w05(): never
{
    throw new RuntimeException("deferred");
}

try {
    w03_rejected_w05();
} catch (RuntimeException $exception) {
    echo $exception->getMessage(), "\n";
}
