--TEST--
Native boxed return preserves strings fetched from an object array property
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
final class W11PPropertyArrayLifetimeProbe
{
    public static int $alive = 0;

    public function __construct()
    {
        self::$alive++;
    }

    public function __destruct()
    {
        self::$alive--;
    }
}

class NativeSectionFile
{
    private array $sections = ['TEST' => ['text' => '', 'probe' => null]];

    public function __construct(string $fileName)
    {
        $this->sections['TEST']['probe'] = new W11PPropertyArrayLifetimeProbe();
        $fp = fopen($fileName, 'rb');
        fgets($fp);
        $section = 'TEST';
        while (!feof($fp)) {
            $line = fgets($fp);
            if ($line === false) {
                break;
            }
            if (preg_match('/^--([_A-Z]+)--/', $line, $matches)) {
                $section = $matches[1];
                $this->sections[$section] = [
                    'text' => '',
                    'probe' => new W11PPropertyArrayLifetimeProbe(),
                ];
                continue;
            }
            $this->sections[$section]['text'] .= $line;
        }
        fclose($fp);
    }

    public function getSection(string $name): array
    {
        if (!isset($this->sections[$name])) {
            throw new Exception("Section $name not found");
        }
        return $this->sections[$name];
    }

}

function property_array_return_lifetime(string $fileName): array
{
    $test = new NativeSectionFile($fileName);
    return $test->getSection('TEST');
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-property-array-return-lifetime.php',
    [__DIR__ . '/w11p_property_array_return_lifetime.phpt'],
    [
        'wave' => 11,
        'function' => 'property_array_return_lifetime',
        'repeat' => 20,
    ],
);
$return = $result['execution']['return_value'];
$alias = $return;
$return['text'][0] = 'n';
$name = trim($alias['text']);
$cow = str_starts_with($name, 'Native boxed')
    && str_starts_with(trim($return['text']), 'native boxed');
unset($return, $alias, $result['execution']['return_value']);
gc_collect_cycles();
printf(
    "%s return=%s closure=%s cow=%s active=%d lifetime_leaks=%d\n",
    $result['status'],
    json_encode($name),
    ($result['execution']['failed_codeunits'] ?? -1) === 0
        && ($result['execution']['performance']['ready_codeunits'] ?? -1)
            === ($result['execution']['performance']['compiled_codeunits'] ?? -2)
        ? 'ready'
        : 'incomplete',
    $cow ? 'yes' : 'no',
    $result['execution']['entry_active_calls'],
    W11PPropertyArrayLifetimeProbe::$alive,
);
?>
--EXPECT--
accepted return="Native boxed return preserves strings fetched from an object array property" closure=ready cow=yes active=0 lifetime_leaks=0
