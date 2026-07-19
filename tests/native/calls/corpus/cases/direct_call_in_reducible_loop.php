<?php
function w05_target(): void { echo 1; }
function w05_case(bool $repeat): void {
loop:
    w05_target();
    if ($repeat) {
        goto loop;
    }
}
