--TEST--
Native Engine keeps an active generation alive across OPcache reset
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.validate_timestamps=0
opcache.file_update_protection=0
--FILE--
<?php

$file = __DIR__ . '/native_engine_reset_generation.tmp.php';
file_put_contents($file, <<<'PHP'
<?php
return static function (): Generator {
    try {
        yield 5;
        yield 8;
    } finally {
        echo "old-finally\n";
    }
};
PHP);

$oldFactory = include $file;
$old = $oldFactory();
echo "old:", $old->current(), "\n";
echo 'cached-before-reset:', opcache_is_script_cached($file) ? 'yes' : 'no', "\n";
var_dump(opcache_reset());

file_put_contents($file, <<<'PHP'
<?php
return static function (): Generator {
    yield 13;
};
PHP);

$code = '$factory = include ' . var_export($file, true) . ';'
    . '$generator = $factory();'
    . 'echo "new:", $generator->current(), "\n";';
$process = proc_open(
    [
        PHP_BINARY,
        '-n',
        '-d', 'opcache.enable_cli=1',
        '-d', 'opcache.validate_timestamps=0',
        '-d', 'opcache.file_update_protection=0',
        '-r', $code,
    ],
    [1 => ['pipe', 'w'], 2 => ['pipe', 'w']],
    $pipes,
);
echo stream_get_contents($pipes[1]);
echo stream_get_contents($pipes[2]);
var_dump(proc_close($process));

$old->next();
echo "old:", $old->current(), "\n";
$old->next();
echo "done\n";
unlink($file);
?>
--CLEAN--
<?php
$file = __DIR__ . '/native_engine_reset_generation.tmp.php';
if (file_exists($file)) {
    unlink($file);
}
?>
--EXPECT--
old:5
cached-before-reset:yes
bool(true)
new:13
int(0)
old:8
old-finally
done
