--TEST--
Native boxed string assignments flow through loop PHIs into switch dispatch
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
function boxed_string_switch_loop_phi(string $argument): string
{
    $configTypes = ['show', 'keep'];
    $configFiles = ['skip', 'php', 'clean', 'out', 'diff', 'exp', 'mem'];
    $config = [];
    $isSwitch = false;
    $switch = substr($argument, 1, 1);
    $repeat = substr($argument, 0, 1) === '-';

    while ($repeat) {
        if (!$isSwitch) {
            $switch = substr($argument, 1, 1);
        }

        $isSwitch = true;

        foreach ($configTypes as $type) {
            if (strpos($switch, '--' . $type) === 0) {
                foreach ($configFiles as $file) {
                    if ($switch === '--' . $type . '-' . $file) {
                        $config[$type][$file] = true;
                        $isSwitch = false;
                        break;
                    }
                }
            }
        }

        if (!$isSwitch) {
            $isSwitch = true;
            break;
        }

        $repeat = false;

        switch ($switch) {
            case '-':
                $switch = $argument;
                if ($switch !== '-') {
                    $repeat = true;
                }
                break;
            case '--help':
                return 'help';
            default:
                return 'miss:' . $switch;
        }
    }

    return 'none';
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-boxed-string-switch-loop-phi.php',
    ['--help'],
    [
        'wave' => 11,
        'function' => 'boxed_string_switch_loop_phi',
        'repeat' => 20,
    ],
);
printf(
    "%s execution=%s exception=%s return=%s closure=%s active=%d\n",
    $result['status'],
    $result['execution']['status'],
    json_encode($result['execution']['exception']),
    json_encode($result['execution']['return_value']),
    ($result['execution']['failed_codeunits'] ?? -1) === 0
        && ($result['execution']['performance']['ready_codeunits'] ?? -1)
            === ($result['execution']['performance']['compiled_codeunits'] ?? -2)
        ? 'ready'
        : 'incomplete',
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted execution=returned exception=false return="help" closure=ready active=0
