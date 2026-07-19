<?php
function w05_return_target() { echo 1; return 1; }
function w05_case(): void { $value = w05_return_target(); echo $value; }
