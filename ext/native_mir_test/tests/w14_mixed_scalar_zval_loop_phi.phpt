--TEST--
Native mixed scalar and ZVAL loop PHIs retain complete machine definitions
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
final class NativeDifferReduction
{
    private $isEqual;

    public function __construct(callable $isEqual)
    {
        $this->isEqual = $isEqual;
    }

    public function calculateCommonSubsequence(array $from, array $to): array
    {
        $cFrom = count($from);
        $cTo = count($to);

        if ($cFrom === 0) {
            return [];
        }

        if ($cFrom === 1) {
            foreach ($to as $toV) {
                if (($this->isEqual)($from[0], $toV)) {
                    return [$toV];
                }
            }

            return [];
        }

        $i = (int) ($cFrom / 2);
        $fromStart = array_slice($from, 0, $i);
        $fromEnd = array_slice($from, $i);
        $llB = $this->commonSubsequenceLength($fromStart, $to);
        $llE = $this->commonSubsequenceLength(
            array_reverse($fromEnd),
            array_reverse($to),
        );
        $jMax = 0;
        $max = 0;

        for ($j = 0; $j <= $cTo; $j++) {
            $m = $llB[$j] + $llE[$cTo - $j];

            if ($m >= $max) {
                $max = $m;
                $jMax = $j;
            }
        }

        $toStart = array_slice($to, 0, $jMax);
        $toEnd = array_slice($to, $jMax);

        return array_merge(
            $this->calculateCommonSubsequence($fromStart, $toStart),
            $this->calculateCommonSubsequence($fromEnd, $toEnd),
        );
    }

    private function commonSubsequenceLength(array $from, array $to): array
    {
        return [];
    }
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w14-mixed-scalar-zval-loop-phi.php',
    [[], []],
    [
        'wave' => 11,
        'function' => 'NativeDifferReduction::calculateCommonSubsequence',
        'repeat' => 20,
    ],
);

printf(
    "%s result=%s vm=%d active=%d closure=%s\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    $result['execution']['vm_handler_calls'],
    $result['execution']['entry_active_calls'],
    ($result['execution']['failed_codeunits'] ?? -1) === 0
        && ($result['execution']['performance']['ready_codeunits'] ?? -1)
            === ($result['execution']['performance']['compiled_codeunits'] ?? -2)
        ? 'ready'
        : 'incomplete',
);
?>
--EXPECT--
accepted result=[] vm=0 active=0 closure=ready
