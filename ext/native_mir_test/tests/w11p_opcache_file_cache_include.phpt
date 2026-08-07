--TEST--
Native include preserves OPcache file-cache-owned opcode storage
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_cache="{TMP}"
opcache.file_cache_only=1
--EXTENSIONS--
opcache
--FILE--
<?php
$include = __DIR__ . '/w11p_opcache_file_cache_include.inc';
file_put_contents(
    $include,
    <<<'PHP'
<?php
function w11p_opcache_file_cache_value(): string
{
    return 'file cache';
}
PHP,
);

try {
    require $include;
    echo w11p_opcache_file_cache_value(), PHP_EOL;
} finally {
    unlink($include);
}
?>
--EXPECT--
file cache
