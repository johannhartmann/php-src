<?php
function w05_variadic_target(...$values): void { echo count($values); }
function w05_case(): void { w05_variadic_target(1); }
