--TEST--
isset() and empty() of properties through the run-time cache slot
--DESCRIPTION--
isset() and empty() of a defined declared property of $this, a CV or a
shared temporary object under a literal name answer from the cached slot;
unset properties with __isset, object values of empty() and dynamic
names keep the general path.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class Type { public $handles = ['a']; public $none = null; public $zero = 0; public $obj; public $ref;
    function __construct() { $this->obj = new stdClass; $x = 1; $this->ref = &$x; } }
class Block { public $type; function __construct() { $this->type = new Type; }
    function probe() { return [isset($this->type->handles), empty($this->type->handles), isset($this->type->none), empty($this->type->zero), empty($this->type->obj), isset($this->type->ref)]; } }
class Lazy { public $p = 1; function __isset($n) { echo "__isset($n) "; return true; } }
$b = new Block; $t = $b->type; $name = 'zero';
for ($i = 0; $i < 3; $i++) {
    echo json_encode([$b->probe(), isset($t->handles), empty($t->none), isset($t->$name), empty($t->$name)]), "\n";
}
$l = new Lazy; unset($l->p);
var_dump(isset($l->p)); echo "|\n";
$t->handles = []; var_dump(empty($t->handles), isset($t->handles));
?>
--EXPECT--
[[true,false,false,true,false,true],true,true,true,true]
[[true,false,false,true,false,true],true,true,true,true]
[[true,false,false,true,false,true],true,true,true,true]
__isset(p) bool(true)
|
bool(true)
bool(true)
