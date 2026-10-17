--TEST--
A fused comparison whose helper decided branches on its boolean inline
--DESCRIPTION--
When a comparison fused into an if/while needs its helper (ArrayAccess isset
and empty, loose and strict comparisons of strings, arrays and objects), the
branch tests the boolean the helper left in the temporary; an exception from
the comparison still leaves through the exception path.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
final class Bag implements ArrayAccess
{
    public function __construct(private array $items, private bool $throw = false) {}
    public function offsetExists($k): bool { if ($this->throw) { throw new RuntimeException("no $k"); } return isset($this->items[$k]); }
    public function offsetGet($k): mixed { return $this->items[$k]; }
    public function offsetSet($k, $v): void { $this->items[$k] = $v; }
    public function offsetUnset($k): void { unset($this->items[$k]); }
}
function isset_branch($bag, $k) { if (isset($bag[$k])) { return 'set'; } return 'unset'; }
function equal_branch($a, $b) { if ($a == $b) { return 'eq'; } return 'ne'; }
function identical_branch($a, $b) { if ($a === $b) { return 'same'; } return 'other'; }
function not_identical_loop(array $list, $stop) { $n = 0; while ($list[$n] !== $stop) { $n++; } return $n; }
function empty_branch($bag, $k) { if (empty($bag[$k])) { return 'empty'; } return 'full'; }
$bag = new Bag(['a' => 1, 'z' => 0]);
$o = new stdClass();
for ($i = 0; $i < 3; $i++) {
    echo isset_branch($bag, 'a'), ' ', isset_branch($bag, 'b'), ' ', empty_branch($bag, 'z'), ' ', empty_branch($bag, 'a'), "\n";
    echo equal_branch("1e3", "1000"), ' ', equal_branch("abc", "abd"), ' ', equal_branch([1, 2], [1, 2]), ' ', equal_branch(null, false), "\n";
    echo identical_branch([1, 'x'], [1, 'x']), ' ', identical_branch($o, $o), ' ', identical_branch($o, new stdClass()), ' ', identical_branch("a$i", "a$i"), "\n";
    echo not_identical_loop(['x', [1], 'y', $o], $o), ' ', not_identical_loop([1.5, 'q', [2]], [2]), "\n";
    try { isset_branch(new Bag([], true), 'k'); } catch (RuntimeException $e) { echo get_class($e), ': ', $e->getMessage(), ' line ', $e->getLine(), "\n"; }
}
--EXPECT--
set unset empty full
eq ne eq eq
same same other same
3 2
RuntimeException: no k line 5
set unset empty full
eq ne eq eq
same same other same
3 2
RuntimeException: no k line 5
set unset empty full
eq ne eq eq
same same other same
3 2
RuntimeException: no k line 5
