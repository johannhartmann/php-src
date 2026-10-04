--TEST--
Native inline FETCH_OBJ_W of cached untyped declared properties
--DESCRIPTION--
FETCH_OBJ_W of a literal property, as in $this->map[$k] = $v or
$this->list[] = $v, addresses a cached untyped declared property inline
through an IS_INDIRECT result. Typed, readonly, reference, magic and
dynamic properties keep the helper's semantics, and separation of shared
arrays is unchanged.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class Store { public $map = []; public $list = []; public array $typed = []; public $ref; public $nested = [];
    public readonly array $ro;
    function __construct() { $a = []; $this->ref = &$a; $this->ro = []; }
    function fill($i) {
        $this->map["k$i"] = $i; $this->list[] = $i; $this->typed[] = $i * 2; $this->ref['r'] = $i;
        $this->nested['a']['b'][] = $i; $this->map['k0'] .= '!';
        try { $this->ro[] = 1; } catch (Error $e) { echo get_class($e), ' '; }
        return json_encode([$this->map, $this->list, $this->typed, $this->ref, $this->nested]);
    }
}
class Magic { private $d = []; function __get($n) { return $this->d; } function poke() { @$this->x['y'] = 1; return 'poked'; } }
#[AllowDynamicProperties]
class Dyn { function add() { $this->dyn['k'][] = 1; return count($this->dyn['k']); } }
$s = new Store; $alias = $s->list;
for ($i = 0; $i < 3; $i++) { echo $s->fill($i), "\n"; }
echo json_encode($alias), ' ', (new Magic)->poke(), ' ', (new Dyn)->add(), "\n";
?>
--EXPECT--
Error [{"k0":"0!"},[0],[0],{"r":0},{"a":{"b":[0]}}]
Error [{"k0":"0!!","k1":1},[0,1],[0,2],{"r":1},{"a":{"b":[0,1]}}]
Error [{"k0":"0!!!","k1":1,"k2":2},[0,1,2],[0,2,4],{"r":2},{"a":{"b":[0,1,2]}}]
[] poked 1
