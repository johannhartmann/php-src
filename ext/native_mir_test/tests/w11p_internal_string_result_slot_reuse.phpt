--TEST--
Native internal string results survive temporary slot reuse across conditional returns
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$source = <<<'PHP'
<?php
function internal_string_result_slot_reuse()
{
    $php = getenv('NATIVE_SLOT_REUSE_PHP');
    $phpEscaped = getenv('NATIVE_SLOT_REUSE_PHP_ESCAPED');
    $cli = false;
    if (file_exists($php) && is_executable($php)) {
        $version = shell_exec("$phpEscaped -n -v");
        if (strstr($version, '(cli)')) {
            $cli = true;
        } elseif (strpos($version, '(cgi')) {
            return 'self';
        }
    }

    if ($cli) {
        $cgiPath = dirname($php) . '/php-cgi';
        if (is_executable($cgiPath)) {
            return $cgiPath;
        }

        $cgiPath = dirname($php, 3) . '/sapi/cgi/php-cgi';
        if (is_executable($cgiPath)) {
            return $cgiPath;
        }
    }

    return 'none';
}
PHP;

$root = sys_get_temp_dir() . '/native-string-result-' . getmypid();
$php = $root . '/build/sapi/cli/php';
$cgi = $root . '/build/sapi/cgi/php-cgi';
mkdir(dirname($php), 0777, true);
mkdir(dirname($cgi), 0777, true);
file_put_contents($php, "#!/bin/sh\nprintf 'PHP 8.6.0 (cli)\\n'\n");
touch($cgi);
chmod($php, 0755);
chmod($cgi, 0755);
putenv('NATIVE_SLOT_REUSE_PHP=' . $php);
putenv('NATIVE_SLOT_REUSE_PHP_ESCAPED=' . escapeshellarg($php));

$result = native_mir_test_compile_execute(
    $source,
    'w11p-internal-string-result-slot-reuse.php',
    [],
    [
        'wave' => 11,
        'function' => 'internal_string_result_slot_reuse',
        'repeat' => 1,
    ],
);
$returned = $result['execution']['return_value'];
$which = $returned === $cgi
    ? 'second'
    : ($returned === dirname($php) . '/php-cgi' ? 'first' : 'other');

putenv('NATIVE_SLOT_REUSE_PHP');
putenv('NATIVE_SLOT_REUSE_PHP_ESCAPED');
unlink($cgi);
unlink($php);
rmdir(dirname($cgi));
rmdir(dirname($php));
rmdir(dirname(dirname($php)));
rmdir(dirname(dirname(dirname($php))));
rmdir($root);

printf(
    "%s return=%s runs=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    json_encode($which),
    $result['execution']['executions'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return="second" runs=1 vm=0 execute_ex=0 handler=0 active=0
