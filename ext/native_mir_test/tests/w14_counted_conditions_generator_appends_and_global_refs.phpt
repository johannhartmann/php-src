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
class W14Block { public $attributes = null; }
function w14_register($block, $key)
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
    $block = new W14Block;
    w14_register($block, 'style');
    w14_register($block, 'anchor');
    w14_register($block, 'style');
    $blocks[] = $block;
}
echo json_encode($blocks[2]), "\n";

function w14_names($class)
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
foreach (w14_names('a b a c b d') as $count => $name) {
    $names[] = $count . $name;
}
echo implode(' ', $names), "\n";

$include = __DIR__ . '/w14_counted_conditions_generator_appends_and_global_refs.inc';
file_put_contents($include, <<<'PHP'
<?php
global $w14_first, $w14_second;
$w14_first = str_contains($_SERVER['W14_MISSING'] ?? '', 'x');
$w14_second = ! $w14_first && ( str_contains( $_SERVER['W14_A'] ?? 'M', 'M' ) || str_contains( $_SERVER['W14_B'] ?? '', 'E' ) );
$w14_third = $w14_second && strlen('abc') === 3;
PHP);
try {
    require $include;
    function w14_read_globals() { global $w14_first, $w14_second; return [$w14_first, $w14_second]; }
    var_dump(w14_read_globals(), $w14_third);
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
