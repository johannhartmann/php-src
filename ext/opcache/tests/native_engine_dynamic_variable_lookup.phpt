--TEST--
Native Engine preserves dynamic local variable lookups with OPcache
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.validate_timestamps=0
opcache.file_update_protection=0
--FILE--
<?php

function native_engine_dynamic_variable_lookup(int $iterations): int
{
    $name = 'value';
    $value = 7;
    $sum = 0;
    for ($index = 0; $index < $iterations; $index++) {
        $sum += $$name;
    }
    return $sum;
}

for ($index = 0; $index < 10; $index++) {
    $result = native_engine_dynamic_variable_lookup(500);
}
echo 'cached:', opcache_is_script_cached(__FILE__) ? 'yes' : 'no', "\n";
echo $result, "\n";
?>
--EXPECT--
cached:yes
3500
