<?php
function w05_followup_target(): int { echo 1; return 7; }
function w05_case(): int
{
    $value = w05_followup_target();
    return $value + 1;
}
