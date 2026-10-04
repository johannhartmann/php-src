--TEST--
Native $closure() calls through the cached closure resolution
--DESCRIPTION--
A $closure(...) site caches the code facts of the closure declaration it
calls and takes the function copy, bound object and scope from each closure:
instances of one declaration with different captured values, closures
created and freed in a loop, rebound closures, static closures, by-reference
captures, arrow functions, recursion, exceptions, and closures of other
declarations, generators and first-class callables at the same site.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class NativeCounter {
    public function __construct(public $base) {}
    public function adder() { return function ($x) { return $this->base + $x; }; }
}

function native_make($n) { return function ($x) use ($n) { return $x * $n; }; }

function native_call($callback, $value) { return $callback($value); }

function native_run()
{
    $out = [];
    $total = 0;
    for ($i = 1; $i <= 4; $i++) {
        $out[] = native_call(native_make($i), 10);
    }
    $plain = function ($x) { return "p$x"; };
    $counter = new NativeCounter(100);
    $bound = $counter->adder();
    $other = Closure::bind($bound, new NativeCounter(200));
    $static = static fn ($x) => $x . '!';
    $sum = 0;
    $acc = function ($x) use (&$sum) { $sum += $x; return $sum; };
    $fib = function ($n) use (&$fib) { return $n < 2 ? $n : $fib($n - 1) + $fib($n - 2); };
    $gen = function ($x) { yield $x; yield $x + 1; };
    $first = strtoupper(...);
    $thrower = function ($x) { throw new RuntimeException("no $x"); };
    foreach ([$plain, $bound, $other, $static, $acc, $acc, $fib, $first] as $callback) {
        $out[] = native_call($callback, 7);
    }
    $out[] = implode(',', iterator_to_array(native_call($gen, 3)));
    try {
        native_call($thrower, 1);
    } catch (RuntimeException $e) {
        $out[] = $e->getMessage();
    }
    for ($i = 0; $i < 50; $i++) {
        $total += native_call(fn ($x) => $x + $i, 1);
    }
    $out[] = $total;
    $out[] = $sum;
    return $out;
}
for ($r = 0; $r < 3; $r++) {
    echo json_encode(native_run()), "\n";
}
?>
--EXPECT--
[10,20,30,40,"p7",107,207,"7!",7,14,13,"7","3,4","no 1",1275,14]
[10,20,30,40,"p7",107,207,"7!",7,14,13,"7","3,4","no 1",1275,14]
[10,20,30,40,"p7",107,207,"7!",7,14,13,"7","3,4","no 1",1275,14]
