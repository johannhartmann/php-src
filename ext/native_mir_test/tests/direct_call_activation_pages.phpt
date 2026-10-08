--TEST--
Direct calls across VM stack pages: leading and heap activations under recursion, unwinding, generators and variadics
--EXTENSIONS--
opcache
--ENV--
ZEND_NATIVE_TIER2_THRESHOLD=3
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
memory_limit=-1
--FILE--
<?php
function rec($n, $pad) { $a = $pad; $b = $pad . $n; return $n === 0 ? strlen($b) : 1 + rec($n - 1, $pad); }
function rec_throw($n) { if ($n === 0) { throw new RuntimeException('deep'); } $x = [$n]; return rec_throw($n - 1) + $x[0]; }
function rec_gen($n) { if ($n === 0) { yield 1; return; } yield from rec_gen($n - 1); yield $n; }
function args(...$a) { return count($a) + array_sum($a); }
function rec_args($n) { return $n === 0 ? 0 : args($n, $n) + rec_args($n - 1); }
for ($round = 0; $round < 3; $round++) {
    echo rec(20000, 'xy'), ' ';
    try { rec_throw(15000); } catch (RuntimeException $e) { echo $e->getMessage(), ' ', count($e->getTrace()), ' '; }
    $s = 0; foreach (rec_gen(300) as $v) { $s += $v; } echo $s, ' ';
    echo rec_args(10000), "\n";
}
echo memory_get_usage() < 64 * 1024 * 1024 ? "ok\n" : "leak\n";
--EXPECT--
20003 deep 15001 45151 100030000
20003 deep 15001 45151 100030000
20003 deep 15001 45151 100030000
ok
