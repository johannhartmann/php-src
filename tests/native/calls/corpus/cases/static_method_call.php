<?php
class W05StaticTarget { public static function run(): void { echo 1; } }
function w05_case(): void { W05StaticTarget::run(); }
