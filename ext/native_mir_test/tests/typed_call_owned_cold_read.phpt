--TEST--
Native typed call releases an owned read its cold path produced
--DESCRIPTION--
An inline property read whose cold helper returns a fresh value (__get() of
an unset declared property) leaves the temporary as its only owner. A typed
call consuming it must free it as the canonical call does: the destructor
runs before the caller continues, and nothing leaks.
--EXTENSIONS--
native_mir_test
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class D {
    function __construct(public $n) {}
    function __destruct() { echo "~{$this->n} "; }
}
class C {
    public $p;
    function __construct($set) {
        if ($set) {
            $this->p = [1, 2];
        } else {
            unset($this->p);
        }
    }
    function __get($n) { return [new D('get')]; }
}
function leaf(array $a, int $x): int {
    if ($x > 100) {
        return $x + 2;
    }
    return $x + 1;
}
function root(object $c, int $x) { return leaf($c->p, $x); }
$k = new C(true);
for ($i = 0; $i < 30; $i++) {
    root($k, $i);
}
$c = new C(false);
for ($i = 0; $i < 3; $i++) {
    echo root($c, $i), "\n";
}
echo "end\n";
?>
--EXPECT--
~get 1
~get 2
~get 3
end
