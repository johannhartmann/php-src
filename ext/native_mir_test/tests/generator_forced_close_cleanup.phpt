--TEST--
Native: a generator's forced close cleans up as the VM's
--DESCRIPTION--
A finally that a destroyed generator runs releases the live ranges at its
FAST_RET before the generator closes as finished, as releasing its CVs may
destroy the generator itself. The cleanup of suspended arguments releases
only SEND temporaries no later opline redefined: the optimizer reuses a
sent temporary for the yield from operand.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class Probe { public function __construct(public string $n) {} public function __destruct() { echo "[d:{$this->n}]\n"; } }
function root() {
    global $gens;
    try {
        yield 1;
    } finally {
        echo count($gens), " in finally\n";
    }
}
function gen($x) {
    global $gens;
    yield from $gens[] = $x ? gen(--$x) : root();
}
$gen = $gens[] = gen(2);
var_dump($gen->current());
unset($gen, $gens);
echo "collect\n";
gc_collect_cycles();
echo "after collect\n";

function id($v) { return $v; }
function reused($p) {
    $a = id(new Probe("arg"));
    yield from $x = (function () use ($p) { try { yield $p; } finally { echo "inner finally\n"; } })();
}
$g = reused('v');
var_dump($g->current());
unset($g);
function pending() {
    try {
        echo strlen(id(new Probe("pending")) ? yield 'y' : 'n'), "\n";
    } finally {
        echo "pending finally\n";
    }
}
$g = pending();
var_dump($g->current());
unset($g);
echo "end\n";
?>
--EXPECT--
int(1)
collect
4 in finally
after collect
string(1) "v"
[d:arg]
inner finally
[d:pending]
string(1) "y"
pending finally
end
