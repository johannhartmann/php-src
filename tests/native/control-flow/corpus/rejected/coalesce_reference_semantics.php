<?php
function w04_coalesce_reference_semantics(mixed &$value) {
    return $value ??= 7;
}
$value = null;
echo w04_coalesce_reference_semantics($value), "\n";
