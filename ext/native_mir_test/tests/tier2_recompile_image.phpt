--TEST--
Tier 2: a hot function recompiles its image mid-request (calls, recursion, exceptions, backtraces, generators)
--EXTENSIONS--
opcache
--ENV--
ZEND_NATIVE_TIER2_THRESHOLD=5
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class Box {
    private array $items = [];
    public function add(string $k, $v): static { $this->items[$k] = $v; return $this; }
    public function get(string $k) { return $this->items[$k] ?? null; }
    public function count(): int { return count($this->items); }
}
function fib($n) { return $n < 2 ? $n : fib($n - 1) + fib($n - 2); }
function thrower($i) { if ($i % 7 === 6) { throw new RuntimeException("boom $i"); } return $i * 2; }
function trace_depth() { return count(debug_backtrace()); }
function wrapper($i) { return trace_depth() + thrower($i) - $i * 2; }
function gen($n) { for ($i = 0; $i < $n; $i++) { yield $i => $i * $i; } }
function sum_gen($n) { $s = 0; foreach (gen($n) as $k => $v) { $s += $v - $k; } return $s; }
function warns($a) { return $a['missing'] ?? @$a['x']; }
$results = [];
for ($round = 0; $round < 3; $round++) {
    $box = new Box;
    for ($i = 0; $i < 20; $i++) { $box->add("k$i", $i)->add("d$i", [$i]); }
    $line = [$box->count(), $box->get('k3'), $box->get('d4'), fib(15), sum_gen(10)];
    $caught = 0; $depths = [];
    for ($i = 0; $i < 20; $i++) {
        try { $depths[] = wrapper($i); } catch (RuntimeException $e) { $caught++; $line[] = $e->getMessage() . '@' . $e->getLine(); }
    }
    $line[] = $caught; $line[] = array_sum($depths); $line[] = warns(['x' => 5]);
    $results[] = $line;
}
var_dump($results[0] === $results[2], $results[1] === $results[2]);
echo json_encode($results[2]), "\n";
--EXPECT--
bool(true)
bool(true)
[40,3,[4],610,240,"boom 6@9","boom 13@9",2,36,5]
