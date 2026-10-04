--TEST--
Native cached property reads preserve non-boolean truthiness
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
class NativeCachedPropertyTruthinessInner
{
    public function value(): bool
    {
        return true;
    }
}

class NativeCachedPropertyTruthinessOuter
{
    protected $condition = false;
    private NativeCachedPropertyTruthinessInner $inner;

    public function __construct()
    {
        $this->inner = new NativeCachedPropertyTruthinessInner();
    }

    public function prime(): void
    {
        $this->condition = true;
        $this->condition &= true;
    }

    public function combined()
    {
        return $this->condition && $this->inner->value();
    }
}

function native_cached_property_truthiness(): array
{
    $outer = new NativeCachedPropertyTruthinessOuter();
    $outer->prime();

    return [
        $outer->combined(),
        $outer->combined(),
        $outer->combined(),
    ];
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'cached-property-truthiness.php',
    [],
    [
        'function' => 'native_cached_property_truthiness',
        'repeat' => 10,
    ],
);

printf(
    "%s return=%s runs=%d active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    $result['execution']['executions'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=[true,true,true] runs=10 active=0
