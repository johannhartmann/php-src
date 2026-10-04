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
$library = __DIR__ . '/dynamic_call_fast_frames_library.inc';
file_put_contents($library, <<<'PHP'
<?php
function native_opt($a, $b = 'd', $c = [1]) { return $a . $b . count($c); }
function native_var($a, ...$rest) { return $a . ':' . implode(',', $rest); }
function native_typed(int $a, string $b = 'x'): string { return $a . $b; }
function native_ret($a): int { return $a; }
function native_none() { }
class NativeBase {
    public static function make($v) { return new static($v); }
    public function __construct(public $v = 0) {}
    public function tag() { return 'base' . $this->v; }
    public static function who() { return static::class; }
}
class NativeChild extends NativeBase {
    public function tag() { return 'child' . parent::tag(); }
    public static function viaSelf() { return self::who() . '/' . static::who(); }
    public static function viaParent() { return parent::who(); }
    public function viaThisStatic() { return static::who(); }
}
class NativeDtor {
    public function __construct(public $name) {}
    public function __destruct() { echo "dtor {$this->name}\n"; }
    public function get() { return $this->name; }
}
PHP);
require $library;
unlink($library);

function native_run($n)
{
    $out = [];
    for ($i = 0; $i < $n; $i++) {
        $out[] = native_opt($i);
        $out[] = native_opt($i, 'e');
        $out[] = native_opt($i, 'e', [1, 2, 3]);
        $out[] = native_var($i);
        $out[] = native_var($i, 1, 2);
        $out[] = native_typed($i);
        $out[] = native_typed("$i", 'y');
        $out[] = native_ret($i + 1);
        $out[] = var_export(native_none(), true);
        native_opt($i);
        $out[] = NativeBase::who();
        $out[] = NativeChild::who();
        $out[] = NativeChild::viaSelf();
        $out[] = NativeChild::viaParent();
        $out[] = NativeChild::make($i)->tag();
        $out[] = (new NativeChild($i))->viaThisStatic();
        $out[] = (new NativeDtor("t$i"))->get();
    }
    return $out;
}
echo implode("\n", native_run(3)), "\n";

for ($i = 0; $i < 3; $i++) {
    try {
        echo native_ret($i < 2 ? $i : 'no'), "\n";
    } catch (TypeError $e) {
        echo get_class($e), ": ", $e->getMessage(), "\n";
    }
    try {
        echo native_typed($i < 2 ? $i : 'no'), "\n";
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
NativeBase
NativeChild
NativeChild/NativeChild
NativeChild
childbase0
NativeChild
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
NativeBase
NativeChild
NativeChild/NativeChild
NativeChild
childbase1
NativeChild
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
NativeBase
NativeChild
NativeChild/NativeChild
NativeChild
childbase2
NativeChild
t2
0
0x
1
1x
TypeError: native_ret(): Return value must be of type int, string returned
TypeError
