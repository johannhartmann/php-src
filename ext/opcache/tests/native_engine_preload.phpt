--TEST--
Native Engine executes preloaded functions and suspended generators
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
opcache.optimization_level=-1
opcache.preload={PWD}/native_engine_preload.inc
--SKIPIF--
<?php
if (PHP_OS_FAMILY === 'Windows') {
    die('skip Preloading is not supported on Windows');
}
?>
--FILE--
<?php

echo "sum:", native_engine_preload_sum(10), "\n";
$generator = native_engine_preload_generator();
echo "generator:", $generator->current(), "\n";
$generator->next();
echo "generator:", $generator->current(), "\n";
$generator->next();
echo "done\n";
?>
--EXPECT--
sum:10
generator:4
generator:7
done
