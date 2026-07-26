--TEST--
Native Engine target images survive an OPcache file-cache process boundary
--EXTENSIONS--
opcache
--INI--
opcache.file_cache="{TMP}/native-engine-file-cache"
--FILE--
<?php

$cache = ini_get('opcache.file_cache') . '-' . getmypid();
mkdir($cache);

function run_native_engine_file_cache_child(string $cache, bool $readOnly): array
{
    $command = [
        PHP_BINARY,
        '-n',
        '-d', 'display_errors=0',
        '-d', 'log_errors=1',
        '-d', 'error_log=/dev/stderr',
        '-d', 'opcache.enable_cli=1',
        '-d', 'opcache.file_update_protection=0',
        '-d', "opcache.file_cache=$cache",
        '-d', 'opcache.file_cache_only=1',
    ];
    if ($readOnly) {
        $command[] = '-d';
        $command[] = 'opcache.file_cache_read_only=1';
    }
    $command[] = __DIR__ . '/native_engine_file_cache.inc';

    $process = proc_open(
        $command,
        [1 => ['pipe', 'w'], 2 => ['pipe', 'w']],
        $pipes,
    );
    $output = stream_get_contents($pipes[1]);
    $error = stream_get_contents($pipes[2]);
    $status = proc_close($process);
    return [$output, $error, $status];
}

foreach ([false, true] as $readOnly) {
    [$output, $error, $status] =
        run_native_engine_file_cache_child($cache, $readOnly);
    echo $output, $error;
    var_dump($status);
}

$files = new RecursiveIteratorIterator(
    new RecursiveDirectoryIterator($cache, FilesystemIterator::SKIP_DOTS),
    RecursiveIteratorIterator::CHILD_FIRST,
);
foreach ($files as $file) {
    $file->isDir() ? rmdir($file->getPathname()) : unlink($file->getPathname());
}
rmdir($cache);
?>
--EXPECT--
42
int(0)
42
int(0)
