<?php
class W05MethodTarget { public function run(): void { echo 1; } }
function w05_case(W05MethodTarget $target): void { $target->run(); }
