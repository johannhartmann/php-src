--TEST--
Native dynamic calls through the cached resolution and fast frame
--DESCRIPTION--
Calls of functions and methods from another file resolve at runtime. Repeated
calls take the cached resolution (function, method and static lookups) and
complete through the fast frame: defaults, variadics, typed parameters and
return types, temporary receivers, discarded results and receiver release
order match the general protocol, and a failing return type or argument type
still throws.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
$library = __DIR__ . '/w14_dynamic_call_fast_frames_library.inc';
file_put_contents($library, <<<'PHP'
<?php
function w14_opt($a, $b = 'd', $c = [1]) { return $a . $b . count($c); }
function w14_var($a, ...$rest) { return $a . ':' . implode(',', $rest); }
function w14_typed(int $a, string $b = 'x'): string { return $a . $b; }
function w14_ret($a): int { return $a; }
function w14_none() { }
class W14Base {
    public static function make($v) { return new static($v); }
    public function __construct(public $v = 0) {}
    public function tag() { return 'base' . $this->v; }
    public static function who() { return static::class; }
}
class W14Child extends W14Base {
    public function tag() { return 'child' . parent::tag(); }
    public static function viaSelf() { return self::who() . '/' . static::who(); }
    public static function viaParent() { return parent::who(); }
    public function viaThisStatic() { return static::who(); }
}
class W14Dtor {
    public function __construct(public $name) {}
    public function __destruct() { echo "dtor {$this->name}\n"; }
    public function get() { return $this->name; }
}
PHP);
require $library;
unlink($library);

function w14_run($n)
{
    $out = [];
    for ($i = 0; $i < $n; $i++) {
        $out[] = w14_opt($i);
        $out[] = w14_opt($i, 'e');
        $out[] = w14_opt($i, 'e', [1, 2, 3]);
        $out[] = w14_var($i);
        $out[] = w14_var($i, 1, 2);
        $out[] = w14_typed($i);
        $out[] = w14_typed("$i", 'y');
        $out[] = w14_ret($i + 1);
        $out[] = var_export(w14_none(), true);
        w14_opt($i);
        $out[] = W14Base::who();
        $out[] = W14Child::who();
        $out[] = W14Child::viaSelf();
        $out[] = W14Child::viaParent();
        $out[] = W14Child::make($i)->tag();
        $out[] = (new W14Child($i))->viaThisStatic();
        $out[] = (new W14Dtor("t$i"))->get();
    }
    return $out;
}
echo implode("\n", w14_run(3)), "\n";

for ($i = 0; $i < 3; $i++) {
    try {
        echo w14_ret($i < 2 ? $i : 'no'), "\n";
    } catch (TypeError $e) {
        echo get_class($e), ": ", $e->getMessage(), "\n";
    }
    try {
        echo w14_typed($i < 2 ? $i : 'no'), "\n";
    } catch (TypeError $e) {
        echo get_class($e), "\n";
    }
}
?>
--EXPECT--
dtor t0
dtor t1
dtor t2
0d1
0e1
0e3
0:
0:1,2
0x
0y
1
NULL
W14Base
W14Child
W14Child/W14Child
W14Child
childbase0
W14Child
t0
1d1
1e1
1e3
1:
1:1,2
1x
1y
2
NULL
W14Base
W14Child
W14Child/W14Child
W14Child
childbase1
W14Child
t1
2d1
2e1
2e3
2:
2:1,2
2x
2y
3
NULL
W14Base
W14Child
W14Child/W14Child
W14Child
childbase2
W14Child
t2
0
0x
1
1x
TypeError: w14_ret(): Return value must be of type int, string returned
TypeError
