<?php
function w05_by_ref(&$value): void { echo $value; }
function w05_case(): void { $value = 1; w05_by_ref($value); }
