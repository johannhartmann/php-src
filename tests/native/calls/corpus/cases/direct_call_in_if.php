<?php
function w05_target(): void { echo 1; }
function w05_case(bool $condition): void { if ($condition) { w05_target(); } }
