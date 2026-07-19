<?php
function w04_object_condition(object $value) {
    if ($value) {
        return 1;
    }
    return 0;
}
echo w04_object_condition(new stdClass()), "\n";
