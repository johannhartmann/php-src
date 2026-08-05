--TEST--
Native boxed property reads do not cross intervening source operations
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
final class W11pPropertyReadOwner
{
    public int $limit = 3;
}

final class W11pPropertyReadCursor
{
    private int $position = 0;

    public function __construct(private W11pPropertyReadOwner $owner) {}

    public function valid(): bool
    {
        return $this->position < $this->owner->limit;
    }

    public function advance(): void
    {
        ++$this->position;
    }
}

function w11p_nested_property_read_liveness(): array
{
    $cursor = new W11pPropertyReadCursor(new W11pPropertyReadOwner());
    $result = [];
    for ($step = 0; $step < 5; ++$step) {
        $result[] = $cursor->valid();
        $cursor->advance();
    }
    return $result;
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-nested-property-read-liveness.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_nested_property_read_liveness',
        'repeat' => 10,
    ],
);
printf(
    "%s return=%s vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=[true,true,true,false,false] vm=0 execute_ex=0 handler=0 active=0
