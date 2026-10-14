--TEST--
Native typed call guards and releases a property read that went through its slot
--DESCRIPTION--
A call between a property read and the typed call that consumes it keeps the
read in its temporary slot. The typed call must still check the value's type,
release the temporary once, and leave a temporary that is the last owner to
the canonical call.
--EXTENSIONS--
native_mir_test
--FILE--
<?php
function leaf(array $values, int $x): array { return $values; }
function leafs(string $s, int $x): string { return $s; }
function other(object $box): int { return 7; }
function clear(object $box): int { $box->values = null; $box->s = null; return 7; }
function root(object $box): array { return leaf($box->values, other($box)); }
function root_clear(object $box): array { return leaf($box->values, clear($box)); }
function roots(object $box): string { return leafs($box->s, other($box)); }
function roots_clear(object $box): string { return leafs($box->s, clear($box)); }

$base = memory_get_usage();
for ($i = 0; $i < 20000; $i++) {
    $box = new stdClass;
    $box->values = range(0, 3);
    $n = count(root($box));
}
echo "n=$n grew=", (memory_get_usage() - $base) > 1000000 ? "yes" : "no", "\n";

for ($i = 0; $i < 3; $i++) {
    $box = new stdClass;
    $box->values = range(0, $i);
    echo json_encode(root_clear($box)), " ", var_export($box->values, true), "\n";
    $box->s = 'interned';
    echo roots($box), "\n";
    $box->s = str_repeat('d', 3 + $i);
    echo roots($box), " ", roots_clear($box), "\n";
}
$box->values = 'not an array';
try {
    root($box);
} catch (TypeError $e) {
    echo get_class($e), "\n";
}
?>
--EXPECT--
n=4 grew=no
[0] NULL
interned
ddd ddd
[0,1] NULL
interned
dddd dddd
[0,1,2] NULL
interned
ddddd ddddd
TypeError
