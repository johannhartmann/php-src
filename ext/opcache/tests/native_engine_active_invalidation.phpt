--TEST--
Native Engine keeps an active generator on its invalidated code generation
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.validate_timestamps=0
opcache.file_update_protection=0
--FILE--
<?php

$file = sys_get_temp_dir()
    . '/native-engine-active-invalidation-' . getmypid() . '.php';

file_put_contents($file, <<<'PHP'
<?php
return static function (): Generator {
    try {
        yield 7;
        yield 11;
    } finally {
        echo "old-finally\n";
    }
};
PHP);

$oldFactory = include $file;
$old = $oldFactory();
echo "old:", $old->current(), "\n";
var_dump(opcache_invalidate($file, true));

file_put_contents($file, <<<'PHP'
<?php
return static function (): Generator {
    yield 13;
};
PHP);

$newFactory = include $file;
$new = $newFactory();
echo "new:", $new->current(), "\n";
$old->next();
echo "old:", $old->current(), "\n";
$old->next();
echo "done\n";
unlink($file);
?>
--EXPECT--
old:7
bool(true)
new:13
old:11
old-finally
done
