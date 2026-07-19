<?php
function w04_try_catch_finally(int $value) {
    try {
        if ($value < 0) {
            throw new RuntimeException("negative");
        }
        return $value;
    } catch (RuntimeException $exception) {
        return 0;
    } finally {
        $value++;
    }
}
echo "try-catch-fixture\n";
