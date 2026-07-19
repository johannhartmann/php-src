<?php
function w05_unpack_target($value): void { echo $value; }
function w05_case(): void { $arguments = [1]; w05_unpack_target(...$arguments); }
