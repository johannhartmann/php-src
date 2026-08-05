--TEST--
Native MIR publishes and executes fresh MAP_JIT images on Darwin arm64
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
if (PHP_OS_FAMILY !== 'Darwin' || php_uname('m') !== 'arm64') {
    die('skip Darwin arm64 only');
}
?>
--FILE--
<?php
for ($value = 1; $value <= 8; $value++) {
    $function = "darwin_publish_{$value}";
    $source = sprintf(
        "<?php\nfunction %s(): int { return %d; }\n",
        $function,
        $value,
    );
    $result = native_mir_test_compile_execute(
        $source,
        "w08-darwin-publish-{$value}.php",
        [],
        ['wave' => 8, 'function' => $function],
    );
    if (
        $result['status'] !== 'accepted'
        || ($result['execution']['return_value'] ?? null) !== $value
        || ($result['execution']['writable_after_publish'] ?? true)
        || !($result['execution']['executable_after_publish'] ?? false)
    ) {
        printf("failure=%s\n", json_encode($result));
        exit(1);
    }
}

echo "published=8 writable=0 executable=1\n";
?>
--EXPECT--
published=8 writable=0 executable=1
