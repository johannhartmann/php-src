<?php
function w05_throwing_target(): void { echo 1; }
function w05_case(): void {
    try { w05_throwing_target(); } catch (Throwable $error) { echo $error; }
}
