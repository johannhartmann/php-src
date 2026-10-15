--TEST--
Native method calls test their receiver as far as its inferred type requires
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class C { public $n; function __construct($n) { $this->n = $n; } function get() { return $this->n; } }
function make($n) { return new C($n); }
function run(int $n) {
    $object = new C($n);
    $sum = 0;
    for ($i = 0; $i < 3; $i++) {
        $sum += $object->get();
        $sum += make($i)->get();
    }
    $alias = &$object;
    $sum += $alias->get();
    $maybe = $n > 5 ? new C(100) : null;
    try {
        $sum += $maybe->get();
    } catch (Error $e) {
        echo $e->getMessage(), "\n";
    }
    return $sum;
}
var_dump(run(2));
?>
--EXPECT--
Call to a member function get() on null
int(11)
