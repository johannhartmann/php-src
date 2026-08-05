--TEST--
Native Engine materializes persistent OPcache literals with request ownership
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.validate_timestamps=0
opcache.file_update_protection=0
--FILE--
<?php

function native_engine_persistent_value_leaf(array $value): array
{
    $value['nested']['value']++;
    return $value;
}

function native_engine_persistent_value_root(): array
{
    $literal = [
        'text' => 'persistent',
        'nested' => ['value' => 10],
    ];
    $copy = $literal;
    $copy['text'] = 'changed';
    $returned = native_engine_persistent_value_leaf($literal);

    return [$literal, $copy, $returned];
}

for ($index = 0; $index < 20; $index++) {
    $result = native_engine_persistent_value_root();
}

echo 'cached=', opcache_is_script_cached(__FILE__) ? 'yes' : 'no', "\n";
echo json_encode($result), "\n";
?>
--EXPECT--
cached=yes
[{"text":"persistent","nested":{"value":10}},{"text":"changed","nested":{"value":10}},{"text":"persistent","nested":{"value":11}}]
