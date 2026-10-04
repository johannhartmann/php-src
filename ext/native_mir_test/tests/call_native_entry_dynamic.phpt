--TEST--
Native dynamic call sites enter recorded targets through their native call entry
--EXTENSIONS--
native_mir_test
--FILE--
<?php
function plain($a, $b = 'B') { return "plain:$a:$b:" . func_num_args(); }
function typed(int $a, string $b = 's') { return "typed:$a:$b"; }
function extra($a) { return 'extra:' . json_encode(func_get_args()); }
function boom($a = 0) { throw new LogicException("boom $a"); }
class H {
    private $p = 'P';
    public function m($x, $y = 'Y') { return "m:$x:$y:{$this->p}"; }
    public static function s($x = 'S') { return "s:$x:" . static::class; }
}
class G extends H {}
$h = new H;
$bound = function ($x, $y = 'cy') { return "closure:$x:$y:" . get_class($this); };
$bound = Closure::bind($bound, $h, H::class);
$free = fn($x, $y = 2) => "arrow:$x:$y";
$targets = ['plain', 'typed', 'extra', [$h, 'm'], ['H', 's'], ['G', 's'], $bound, $free, 'strtoupper'];
for ($round = 0; $round < 4; $round++) {
    foreach ($targets as $i => $cb) {
        $one = [$i + 1];
        $two = [$i + 1, "v$round"];
        echo call_user_func_array($cb, $one), ' | ', call_user_func_array($cb, $two), ' | ', $cb(...$one), "\n";
    }
    echo call_user_func_array('extra', [1, 2, 3]), "\n";
    try { echo call_user_func_array('typed', ['x']), "\n"; } catch (TypeError $e) { echo "TypeError\n"; }
    try { call_user_func_array('boom', [$round]); } catch (LogicException $e) { echo $e->getMessage(), ' ', $e->getTrace()[0]['function'], "\n"; }
    echo call_user_func_array('plain', ['named' => 1, 'b' => 2] ? ['a' => 'na', 'b' => 'nb'] : []), "\n";
}
--EXPECTF--
plain:1:B:1 | plain:1:v0:2 | plain:1:B:1
typed:2:s | typed:2:v0 | typed:2:s
extra:[3] | extra:[3,"v0"] | extra:[3]
m:4:Y:P | m:4:v0:P | m:4:Y:P
s:5:H | s:5:H | s:5:H
s:6:G | s:6:G | s:6:G
closure:7:cy:H | closure:7:v0:H | closure:7:cy:H
arrow:8:2 | arrow:8:v0 | arrow:8:2
9 | 
Fatal error: Uncaught ArgumentCountError: strtoupper() expects exactly 1 argument, 2 given in %s:21
Stack trace:
#0 %s(21): strtoupper(9, 'v0')
#1 {main}
  thrown in %s on line 21
