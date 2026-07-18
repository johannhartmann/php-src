<?php

function w03_rejected_objects(): object
{
    return new stdClass();
}

echo get_class(w03_rejected_objects()), "\n";
