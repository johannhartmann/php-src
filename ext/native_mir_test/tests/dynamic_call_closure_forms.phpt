--TEST--
Native: $f(...) of fake closures, call results and unpacked arguments takes the dynamic fast path
--DESCRIPTION--
First-class callables of methods ($object->method(...), Class::method(...),
trait methods), closures given a call result (SEND_VAR_NO_REF_EX) and
closures given unpacked arguments take the recorded dynamic target. Their
frames behave as the VM's: backtraces name the method, static variables are
the method's, a by-reference parameter of a call result notices, unpacked
references, named and extra arguments, defaults and errors.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
namespace NativeDynamicForms;

trait Named {
    private string $tag = 'trait';
    public function describe($x) {
        static $calls = 0;
        $calls++;
        $frame = debug_backtrace(DEBUG_BACKTRACE_IGNORE_ARGS)[0];
        return self::class . '/' . static::class . '/' . $this->tag . '/' . ($frame['class'] ?? '-') . '::' . $frame['function'] . "#$calls:$x";
    }
}
class A { use Named; }
class B { use Named; public function __construct() { $this->tag = 'b'; } }
class Bag {
    public array $log = [];
    public function __destruct() { echo "destruct bag\n"; }
    public function add(...$items) { foreach ($items as $k => $v) $this->log[] = "$k=$v"; return count($items); }
    public function swap(&$a, &$b) { [$a, $b] = [$b, $a]; return "$a,$b"; }
    public static function twice($v) { return static::class . ':' . ($v * 2); }
    public function over() { return ['o']; }
    public function build($concrete) { return $concrete($this, $this->over()); }
}
class SubBag extends Bag {}
function value() { return 'v'; }
function &ref() { static $r = 'r'; return $r; }

function run_fake($f, $x) { return $f($x); }
function run_two($f, $a, $b) { return $f($a, $b); }
function run_unpack($f, array $args) { return $f(...$args); }
function run_result($f) { return $f(value()); }
function run_ref_result($f) { return $f(ref()); }

$a = new A; $b = new B;
$fa = $a->describe(...); $fb = $b->describe(...);
$twice = SubBag::twice(...);
for ($round = 0; $round < 3; $round++) {
    $bag = new Bag;
    $add = $bag->add(...);
    $out = [];
    $out[] = run_fake($fa, $round);
    $out[] = run_fake($fb, $round);
    $out[] = run_fake($twice, $round + 1);
    $out[] = run_fake($add, 'x');
    $out[] = run_two($add, 'p', 'q');
    $out[] = run_unpack($add, []);
    $out[] = run_unpack($add, [1, 2, 3]);
    $out[] = run_unpack($add, ['k' => 'named', 'j' => 'other']);
    $out[] = run_unpack(fn($a, $b = 'dflt', ...$rest) => "$a|$b|" . implode(',', $rest), ['b' => 'B', 'a' => 'A']);
    $out[] = run_unpack(fn($a, $b = 'dflt', ...$rest) => "$a|$b|" . implode(',', $rest), [1, 2, 3, 4]);
    $out[] = run_unpack(fn($a, $b = 'dflt') => "$a|$b", [7]);
    $x = 'L'; $y = 'R';
    $args = [&$x, &$y];
    $out[] = run_unpack($bag->swap(...), $args) . "/$x$y";
    $out[] = run_result(fn($v) => "res:$v");
    $out[] = run_ref_result(fn($v) => "ref:$v");
    $out[] = $bag->build(fn($app, $o) => get_class($app) . ':' . implode($o));
    $out[] = $bag->build(fn($app) => 'one:' . func_num_args());
    echo implode(' ', $out), "\n";
    echo implode(',', $bag->log), "\n";
    unset($add, $bag);
}
$g = function &(&$v) { $v .= '!'; return $v; };
try {
    echo run_result($g), "\n";
} catch (\Error $e) {
    echo get_class($e), ': ', $e->getMessage(), "\n";
}
echo run_ref_result($g), "\n";
try {
    run_unpack(fn($a) => $a, ['a' => 1, 'a2' => 2]);
} catch (\Error $e) {
    echo get_class($e), ': ', $e->getMessage(), "\n";
}
try {
    run_unpack(fn($a, $b) => $a, [1]);
} catch (\ArgumentCountError $e) {
    echo get_class($e), ': ', $e->getMessage(), "\n";
}
function gen() { yield 1; yield 2; }
echo run_unpack(fn(...$v) => implode('+', $v), [...gen()]), "\n";
?>
--EXPECTF--
NativeDynamicForms\A/NativeDynamicForms\A/trait/NativeDynamicForms\A::describe#1:0 NativeDynamicForms\B/NativeDynamicForms\B/b/NativeDynamicForms\B::describe#1:0 NativeDynamicForms\SubBag:2 1 2 0 3 2 A|B| 1|2|3,4 7|dflt R,L/RL res:v ref:r NativeDynamicForms\Bag:o one:2
0=x,0=p,1=q,0=1,1=2,2=3,k=named,j=other
destruct bag
NativeDynamicForms\A/NativeDynamicForms\A/trait/NativeDynamicForms\A::describe#2:1 NativeDynamicForms\B/NativeDynamicForms\B/b/NativeDynamicForms\B::describe#2:1 NativeDynamicForms\SubBag:4 1 2 0 3 2 A|B| 1|2|3,4 7|dflt R,L/RL res:v ref:r NativeDynamicForms\Bag:o one:2
0=x,0=p,1=q,0=1,1=2,2=3,k=named,j=other
destruct bag
NativeDynamicForms\A/NativeDynamicForms\A/trait/NativeDynamicForms\A::describe#3:2 NativeDynamicForms\B/NativeDynamicForms\B/b/NativeDynamicForms\B::describe#3:2 NativeDynamicForms\SubBag:6 1 2 0 3 2 A|B| 1|2|3,4 7|dflt R,L/RL res:v ref:r NativeDynamicForms\Bag:o one:2
0=x,0=p,1=q,0=1,1=2,2=3,k=named,j=other
destruct bag

Notice: Only variables should be passed by reference in %s on line 31
v!
r!
Error: Unknown named parameter $a2
ArgumentCountError: Too few arguments to function {closure:%s:76}(), 1 passed in %s on line 30 and exactly 2 expected
1+2
