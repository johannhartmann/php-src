--TEST--
Native property write of a literal value
--DESCRIPTION--
$this->prop = <literal> writes the declared property inline like other
values: untyped and int-typed properties, coercion and type errors of
typed properties, readonly, hooked and magic properties, and the release
of an overwritten counted value keep stock semantics.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class D { function __destruct() { echo "~D "; } }
class P {
    public $a; public $b = 'x'; public int $n = 0; public ?string $s = null; public float $f = 0.0;
    public readonly int $ro;
    public $hooked { set { $this->hooked_raw = "hooked:$value"; } get { return $this->hooked_raw ?? null; } }
    public $hooked_raw;
    function fill() {
        $this->a = null; $this->b = [1, 2]; $this->n = 5; $this->s = 'str'; $this->f = 2;
        $this->a = new D; $this->a = 'replaced';
        $this->hooked = 'v';
        return [$this->a, $this->b, $this->n, $this->s, $this->f, $this->hooked];
    }
    function bad() { $this->n = 'nope'; }
    function ro() { $this->ro = 1; $this->ro = 2; }
}
class M { private $data = []; function __set($k, $v) { $this->data[$k] = $v; } function set() { $this->undeclared = 'magic'; return $this->data; } }
for ($i = 0; $i < 3; $i++) {
    $p = new P;
    echo json_encode($p->fill()), "|\n";
    try { $p->bad(); } catch (TypeError $e) { echo "TypeError|\n"; }
    try { $p->ro(); } catch (Error $e) { echo get_class($e), "|\n"; }
    echo json_encode((new M)->set()), "|\n";
}
?>
--EXPECT--
~D ["replaced",[1,2],5,"str",2,"hooked:v"]|
TypeError|
Error|
{"undeclared":"magic"}|
~D ["replaced",[1,2],5,"str",2,"hooked:v"]|
TypeError|
Error|
{"undeclared":"magic"}|
~D ["replaced",[1,2],5,"str",2,"hooked:v"]|
TypeError|
Error|
{"undeclared":"magic"}|
