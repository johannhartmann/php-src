--TEST--
Native VM negative-proof counters have a real positive calibration
--EXTENSIONS--
native_mir_test
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is unavailable');
}
?>
--FILE--
<?php
$baseline = native_mir_test_compile_execute(
    '<?php function native_vm_probe_baseline(int $value): int { return $value + 1; }',
    __FILE__ . '.baseline',
    [41],
    ['wave' => 11, 'function' => 'native_vm_probe_baseline'],
);
$calibrated = native_mir_test_compile_execute(
    '<?php function native_vm_probe_calibrated(int $value): int { return $value + 1; }',
    __FILE__ . '.calibrated',
    [41],
    [
        'wave' => 11,
        'function' => 'native_vm_probe_calibrated',
        'vm_probe_calibration' => true,
    ],
);
$sparse = native_mir_test_compile_execute(
    '<?php function native_vm_probe_sparse(int $value): int { return $value; }',
    __FILE__ . '.sparse',
    [1 => 41],
    ['wave' => 11, 'function' => 'native_vm_probe_sparse'],
);
$named = native_mir_test_compile_execute(
    '<?php function native_vm_probe_named(int $value): int { return $value; }',
    __FILE__ . '.named',
    ['value' => 41],
    ['wave' => 11, 'function' => 'native_vm_probe_named'],
);

foreach (['baseline' => $baseline, 'calibrated' => $calibrated] as $name => $result) {
    $execution = $result['execution'];
	$positive = $name !== 'calibrated' || (
		$execution['vm_handler_calls'] > 0
		&& $execution['execute_ex_calls'] > 0
		&& $execution['opline_handler_calls'] > 0
	);
    printf(
        "%s %s return=%d vm=%d execute_ex=%d opline=%d positive=%s\n",
        $name,
        $result['status'],
        $execution['return_value'],
        $execution['vm_handler_calls'],
        $execution['execute_ex_calls'],
        $execution['opline_handler_calls'],
		$positive ? 'yes' : 'no',
    );
}
printf("sparse=%s named=%s\n", $sparse['status'], $named['status']);
?>
--EXPECTF--
baseline accepted return=42 vm=0 execute_ex=0 opline=0 positive=yes
calibrated accepted return=42 vm=%d execute_ex=%d opline=%d positive=yes
sparse=error named=error
