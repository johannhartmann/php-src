--TEST--
Native multiway dispatch resolves literal match operands without a frame slot
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
function literal_match_case(): string
{
    return match (3) {
        1, 2, 3, 4, 5 => 'case',
        default => 'default',
    };
}

function literal_match_default(): string
{
    return match (true) {
        1, 2, 3, 4, 5 => 'case',
        default => 'default',
    };
}
PHP;

foreach (['literal_match_case', 'literal_match_default'] as $function) {
    $result = native_mir_test_compile_execute(
        $source,
        'w12-literal-match-dispatch.php',
        [],
        [
            'wave' => 11,
            'function' => $function,
            'repeat' => 1,
        ],
    );
    printf(
        "%s %s return=%s vm=%d active=%d\n",
        $result['status'],
        $function,
        $result['execution']['return_value'],
        $result['execution']['vm_handler_calls'],
        $result['execution']['entry_active_calls'],
    );
}
?>
--EXPECT--
accepted literal_match_case return=case vm=0 active=0
accepted literal_match_default return=default vm=0 active=0
