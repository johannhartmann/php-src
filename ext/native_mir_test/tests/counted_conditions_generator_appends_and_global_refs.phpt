--TEST--
Native counted conditions, appends across yields and global references
--DESCRIPTION--
A register-held property read tested by JMPZ/JMPNZ owns a reference that the
helper must release, or the array stays shared and leaks when it is written.
An append that separates an array in a generator replaces the slot's pointer;
the suspend must not store the stale register copy back. A top-level include
binding a global keeps the reference in its CV slot when a short-circuit
condition reads it.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class NativeBlock { public $attributes = null; }
function native_register($block, $key)
{
    if (!$block->attributes) {
        $block->attributes = array();
    }
    if (!array_key_exists($key, $block->attributes)) {
        $block->attributes[$key] = array('type' => 'object');
    }
}
$blocks = [];
for ($i = 0; $i < 3; $i++) {
    $block = new NativeBlock;
    native_register($block, 'style');
    native_register($block, 'anchor');
    native_register($block, 'style');
    $blocks[] = $block;
}
echo json_encode($blocks[2]), "\n";

function native_names($class)
{
    $seen = array();
    $at = 0;
    while ($at < strlen($class)) {
        $at += strspn($class, ' ', $at);
        if ($at >= strlen($class)) {
            return;
        }
        $length = strcspn($class, ' ', $at);
        $name = substr($class, $at, $length);
        $at += $length;
        if (in_array($name, $seen, true)) {
            continue;
        }
        $seen[] = $name;
        yield count($seen) => $name;
    }
}
$names = [];
foreach (native_names('a b a c b d') as $count => $name) {
    $names[] = $count . $name;
}
echo implode(' ', $names), "\n";

$include = __DIR__ . '/counted_conditions_generator_appends_and_global_refs.inc';
file_put_contents($include, <<<'PHP'
<?php
global $native_first, $native_second;
$native_first = str_contains($_SERVER['NATIVE_MISSING'] ?? '', 'x');
$native_second = ! $native_first && ( str_contains( $_SERVER['NATIVE_A'] ?? 'M', 'M' ) || str_contains( $_SERVER['NATIVE_B'] ?? '', 'E' ) );
$native_third = $native_second && strlen('abc') === 3;
PHP);
try {
    require $include;
    function native_read_globals() { global $native_first, $native_second; return [$native_first, $native_second]; }
    var_dump(native_read_globals(), $native_third);
} finally {
    unlink($include);
}
?>
--EXPECT--
{"attributes":{"style":{"type":"object"},"anchor":{"type":"object"}}}
1a 2b 3c 4d
array(2) {
  [0]=>
  bool(false)
  [1]=>
  bool(true)
}
bool(true)
