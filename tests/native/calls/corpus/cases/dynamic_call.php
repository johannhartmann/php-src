<?php
function w05_dynamic_target(): void { echo 1; }
function w05_case(): void { $callable = "w05_dynamic_target"; $callable(); }
