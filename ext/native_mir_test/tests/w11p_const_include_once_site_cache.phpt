--TEST--
Native constant include_once sites preserve request-local once semantics
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$includeFile = __DIR__ . '/w11p_const_include_once_site_cache_include.inc';
$requireFile = __DIR__ . '/w11p_const_include_once_site_cache_require.inc';
file_put_contents(
    $includeFile,
    "<?php \$GLOBALS['w11p_include_once_count']++; return 11;\n",
);
file_put_contents(
    $requireFile,
    "<?php \$GLOBALS['w11p_require_once_count']++; return 13;\n",
);

$GLOBALS['w11p_include_once_count'] = 0;
$GLOBALS['w11p_require_once_count'] = 0;
$source = sprintf(
    <<<'PHP'
<?php
function const_include_once_site_cache(): array
{
    for ($index = 0; $index < 25; $index++) {
        include_once %s;
        require_once %s;
    }
    return [
        $GLOBALS['w11p_include_once_count'],
        $GLOBALS['w11p_require_once_count'],
    ];
}
PHP,
    var_export($includeFile, true),
    var_export($requireFile, true),
);

$result = native_mir_test_compile_execute(
    $source,
    'w11p-const-include-once-site-cache.php',
    [],
    [
        'wave' => 11,
        'function' => 'const_include_once_site_cache',
        'repeat' => 20,
    ],
);
printf(
    "%s return=%s vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value'] ?? null),
    $result['execution']['vm_handler_calls'] ?? -1,
    $result['execution']['execute_ex_calls'] ?? -1,
    $result['execution']['opline_handler_calls'] ?? -1,
    $result['execution']['entry_active_calls'] ?? -1,
);
?>
--CLEAN--
<?php
@unlink(__DIR__ . '/w11p_const_include_once_site_cache_include.inc');
@unlink(__DIR__ . '/w11p_const_include_once_site_cache_require.inc');
?>
--EXPECT--
accepted return=[1,1] vm=0 execute_ex=0 handler=0 active=0
