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
final class NativePropertyReadOwner
{
    public int $limit = 3;
}

final class NativePropertyReadCursor
{
    private int $position = 0;

    public function __construct(private NativePropertyReadOwner $owner) {}

    public function valid(): bool
    {
        return $this->position < $this->owner->limit;
    }

    public function advance(): void
    {
        ++$this->position;
    }
}

function native_nested_property_read_liveness(): array
{
    $cursor = new NativePropertyReadCursor(new NativePropertyReadOwner());
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
    'nested-property-read-liveness.php',
    [],
    [
        'function' => 'native_nested_property_read_liveness',
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
