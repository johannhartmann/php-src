--TEST--
Native call sites whose sends directly follow their Init chain the universal paths
--DESCRIPTION--
A fast call site whose phases are only its own sends runs no mode tests: the
universal path of each phase (a site not yet armed, a miss) continues in the
next phase's. The callees live in another file, so every site takes the
source-call protocol; the first round runs the universal paths, later rounds
the fast ones. Undefined arguments, a warning turned into an exception during
a send, method sites (second level), constructors, static, by-reference,
dynamic and variadic targets behave as in the VM in both. An undefined
argument whose warning throws is null when the unfinished-call cleanup
releases the sent arguments (a stale slot was freed before).
--EXTENSIONS--
opcache
native_mir_test
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
$library = __DIR__ . '/call_phases_chained.inc';
file_put_contents($library, <<<'PHP'
<?php
function pair($a, $b) { return "$a|$b"; }
function none() { return 'none'; }
function three($a, $b, $c) { return "$a|$b|$c"; }
function grow(&$value, $by) { $value .= $by; return strlen($value); }
function rest($a, ...$more) { return $a . ':' . implode(',', $more); }
class Tracked {
    public function __construct(public string $name) {}
    public function __destruct() { echo "destroy {$this->name}\n"; }
}
class Base {
    public function __construct(public $a = null, public $b = null) {}
    public function join($x, $y) { return static::class . "($x,$y)"; }
    public static function make($x, $y) { return new static($x, $y); }
}
class Left extends Base {}
class Right extends Base {
    public function join($x, $y) { return "right($y,$x)"; }
}
PHP);
require $library;

function run(int $round): void {
    $x = "x$round";
    $n = $round * 10;
    echo pair($x, 17), "\n";
    echo pair($n, 2.5), "\n";
    echo none(), "\n";
    echo three($x, null, true), "\n";
    echo @pair($missing, $x), "\n";
    $text = 'ab';
    echo grow($text, $x), " $text\n";
    echo rest($x), ' ', rest($x, 1, $n), "\n";
    $f = 'pair';
    echo $f($n, $x), "\n";
    $object = new Base($x, $n);
    echo $object->a, '|', $object->b, "\n";
    foreach ([new Left, new Right, new Left] as $receiver) {
        echo $receiver->join($x, $n), "\n";
    }
    $made = Left::make($n, $x);
    echo get_class($made), ' ', $made->a, ' ', $made->b, "\n";
    $tracked = new Tracked("t$round");
    set_error_handler(function ($severity, $message) {
        throw new ErrorException($message, 0, $severity);
    });
    $calls = [
        fn() => pair($tracked, $undefined),
        fn() => $f($tracked, $undefined),
        fn() => (new Right)->join($tracked, $undefined),
        fn() => new Base($tracked, $undefined),
        fn() => Left::make($tracked, $undefined),
    ];
    foreach ($calls as $index => $call) {
        try {
            echo $call(), "\n";
        } catch (ErrorException $e) {
            echo $index, ' ', get_class($e), ': ', $e->getMessage(), "\n";
        }
    }
    restore_error_handler();
    unset($calls);
    unset($tracked);
    echo "end $round\n";
}

for ($round = 0; $round < 3; $round++) {
    run($round);
}
?>
--CLEAN--
<?php
@unlink(__DIR__ . '/call_phases_chained.inc');
?>
--EXPECT--
x0|17
0|2.5
none
x0||1
|x0
4 abx0
x0: x0:1,0
0|x0
x0|0
Left(x0,0)
right(0,x0)
Left(x0,0)
Left 0 x0
0 ErrorException: Undefined variable $undefined
1 ErrorException: Undefined variable $undefined
2 ErrorException: Undefined variable $undefined
3 ErrorException: Undefined variable $undefined
4 ErrorException: Undefined variable $undefined
end 0
destroy t0
x1|17
10|2.5
none
x1||1
|x1
4 abx1
x1: x1:1,10
10|x1
x1|10
Left(x1,10)
right(10,x1)
Left(x1,10)
Left 10 x1
0 ErrorException: Undefined variable $undefined
1 ErrorException: Undefined variable $undefined
2 ErrorException: Undefined variable $undefined
3 ErrorException: Undefined variable $undefined
4 ErrorException: Undefined variable $undefined
end 1
destroy t1
x2|17
20|2.5
none
x2||1
|x2
4 abx2
x2: x2:1,20
20|x2
x2|20
Left(x2,20)
right(20,x2)
Left(x2,20)
Left 20 x2
0 ErrorException: Undefined variable $undefined
1 ErrorException: Undefined variable $undefined
2 ErrorException: Undefined variable $undefined
3 ErrorException: Undefined variable $undefined
4 ErrorException: Undefined variable $undefined
end 2
destroy t2
