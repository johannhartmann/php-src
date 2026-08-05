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
class W11pCachedPropertyTruthinessInner
{
    public function value(): bool
    {
        return true;
    }
}

class W11pCachedPropertyTruthinessOuter
{
    protected $condition = false;
    private W11pCachedPropertyTruthinessInner $inner;

    public function __construct()
    {
        $this->inner = new W11pCachedPropertyTruthinessInner();
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

function w11p_cached_property_truthiness(): array
{
    $outer = new W11pCachedPropertyTruthinessOuter();
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
    'w11p-cached-property-truthiness.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_cached_property_truthiness',
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
