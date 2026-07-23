--TEST--
Native MIR W11 includes through a user stream wrapper with native reentry
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
class W11IncludeStream
{
    public $context;
    private string $source = '';
    private int $offset = 0;

    public function stream_open(
        string $path,
        string $mode,
        int $options,
        ?string &$openedPath,
    ): bool {
        $this->source = '<?php return 42;';
        $this->offset = 0;
        $openedPath = $path;
        return true;
    }

    public function stream_read(int $length): string
    {
        $chunk = substr($this->source, $this->offset, $length);
        $this->offset += strlen($chunk);
        return $chunk;
    }

    public function stream_eof(): bool
    {
        return $this->offset >= strlen($this->source);
    }

    public function stream_stat(): array
    {
        return ['size' => strlen($this->source)];
    }

    public function url_stat(string $path, int $flags): array
    {
        return ['size' => strlen('<?php return 42;')];
    }

    public function stream_set_option(
        int $option,
        int $argument1,
        int $argument2,
    ): bool {
        return false;
    }
}

stream_wrapper_register('w11', W11IncludeStream::class);

$source = <<<'PHP'
<?php
function w11_stream_include(string $uri): int {
    return include $uri;
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11-stream-wrapper.php',
    ['w11://dynamic-unit'],
    ['wave' => 11, 'function' => 'w11_stream_include'],
);
printf(
    "%s return=%s vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value'] ?? null),
    $result['execution']['vm_handler_calls'] ?? -1,
    $result['execution']['execute_ex_calls'] ?? -1,
    $result['execution']['opline_handler_calls'] ?? -1,
);

stream_wrapper_unregister('w11');
?>
--EXPECT--
accepted return=42 vm=0 execute_ex=0 handler=0
