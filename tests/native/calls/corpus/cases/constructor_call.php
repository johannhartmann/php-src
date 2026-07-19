<?php
class W05ConstructorTarget { public function __construct() { echo 1; } }
function w05_case(): void { new W05ConstructorTarget(); }
