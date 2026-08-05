--TEST--
Native Engine code and OPcache generations remain valid across fork
--EXTENSIONS--
opcache
pcntl
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.validate_timestamps=0
opcache.file_update_protection=0
--SKIPIF--
<?php
if (!function_exists('pcntl_fork')) {
    die('skip pcntl is required');
}
?>
--FILE--
<?php

function native_engine_fork_leaf(int $value): int
{
    return $value + 1;
}

function native_engine_fork_sum(int $count): int
{
    $value = 0;
    for ($index = 0; $index < $count; $index++) {
        $value = native_engine_fork_leaf($value);
    }
    return $value;
}

echo 'cached:', opcache_is_script_cached(__FILE__) ? 'yes' : 'no', "\n";
echo "parent-before:", native_engine_fork_sum(5), "\n";
fflush(STDOUT);
$pid = pcntl_fork();
if ($pid === 0) {
    echo "child-before:", native_engine_fork_sum(7), "\n";
    var_dump(opcache_reset());
    echo "child-after:", native_engine_fork_sum(11), "\n";
    exit(0);
}
if ($pid < 0) {
    throw new RuntimeException('fork failed');
}
pcntl_waitpid($pid, $status);
echo "child-status:", pcntl_wexitstatus($status), "\n";
echo "parent-after:", native_engine_fork_sum(13), "\n";
?>
--EXPECT--
cached:yes
parent-before:5
child-before:7
bool(true)
child-after:11
child-status:0
parent-after:13
