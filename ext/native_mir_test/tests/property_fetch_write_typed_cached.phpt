--TEST--
Native write fetches of typed properties take the cached path with the VM's flag checks
--DESCRIPTION--
FETCH_OBJ_W/RW/UNSET of a declared typed property whose class and offset
the run-time cache holds address it directly and apply the fetch flags
(dimension write auto-initialization, by-reference fetch) as
zend_fetch_property_address() does, including a reference holding false.
Readonly, asymmetric-visibility, uninitialized, nested and dynamic
properties keep their PHP behaviour.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class Node {
    public array $items = [];
    public ?array $maybe = null;
    public int $count = 0;
    public ?int $nullable = null;
    public $plain = null;
    public string|false $sf = false;
    public readonly array $ro;
    public private(set) array $asym = [];
    public object $obj;
    public function __construct() { $this->ro = [1]; $this->obj = new stdClass; }
    function fill($n) {
        for ($i = 0; $i < $n; $i++) {
            $this->items[] = $i;
            $this->items['k' . $i] = $i * 2;
            $this->maybe[] = $i;
            $this->plain[] = $i;
            $this->asym[] = $i;
            $r = &$this->count;
            $r++;
            unset($r);
            $this->obj->x[] = $i;
            unset($this->items['k0']);
        }
        return $this;
    }
    function refs() {
        $a = &$this->nullable;
        $a = 5;
        $b = &$this->maybe;
        $b[] = 'ref';
        return [$a, $b];
    }
}
function outside(Node $n) {
    try { $n->ro[] = 2; } catch (Error $e) { echo get_class($e), ": ", $e->getMessage(), "\n"; }
    try { $n->asym[] = 2; } catch (Error $e) { echo get_class($e), ": ", $e->getMessage(), "\n"; }
    try { $n->sf[] = 1; } catch (Error $e) { echo get_class($e), ": ", $e->getMessage(), "\n"; }
    try { $n->count[] = 1; } catch (Error $e) { echo get_class($e), ": ", $e->getMessage(), "\n"; }
    $n->sf = 'str';
    $s = &$n->sf;
    $s = false;
    try { $n->sf[] = 1; } catch (Error $e) { echo get_class($e), ": ", $e->getMessage(), "\n"; }
    unset($s);
    try { $x = &$n->ro; } catch (Error $e) { echo get_class($e), ": ", $e->getMessage(), "\n"; }
    $n->ro2 = 1;
}
$n = new Node;
$n->fill(3);
var_dump($n->refs());
@outside($n);
outside(new Node);
var_dump($n);
?>
--EXPECTF--
array(2) {
  [0]=>
  int(5)
  [1]=>
  array(4) {
    [0]=>
    int(0)
    [1]=>
    int(1)
    [2]=>
    int(2)
    [3]=>
    string(3) "ref"
  }
}
Error: Cannot indirectly modify readonly property Node::$ro
Error: Cannot indirectly modify private(set) property Node::$asym from global scope
TypeError: Cannot auto-initialize an array inside property Node::$sf of type string|false
Error: Cannot use a scalar value as an array
TypeError: Cannot auto-initialize an array inside property Node::$sf of type string|false
Error: Cannot indirectly modify readonly property Node::$ro
Error: Cannot indirectly modify readonly property Node::$ro
Error: Cannot indirectly modify private(set) property Node::$asym from global scope
TypeError: Cannot auto-initialize an array inside property Node::$sf of type string|false
Error: Cannot use a scalar value as an array
TypeError: Cannot auto-initialize an array inside property Node::$sf of type string|false
Error: Cannot indirectly modify readonly property Node::$ro

Deprecated: Creation of dynamic property Node::$ro2 is deprecated in %s on line 47
object(Node)#1 (10) {
  ["items"]=>
  array(5) {
    [0]=>
    int(0)
    [1]=>
    int(1)
    ["k1"]=>
    int(2)
    [2]=>
    int(2)
    ["k2"]=>
    int(4)
  }
  ["maybe"]=>
  array(4) {
    [0]=>
    int(0)
    [1]=>
    int(1)
    [2]=>
    int(2)
    [3]=>
    string(3) "ref"
  }
  ["count"]=>
  int(3)
  ["nullable"]=>
  int(5)
  ["plain"]=>
  array(3) {
    [0]=>
    int(0)
    [1]=>
    int(1)
    [2]=>
    int(2)
  }
  ["sf"]=>
  bool(false)
  ["ro"]=>
  array(1) {
    [0]=>
    int(1)
  }
  ["asym"]=>
  array(3) {
    [0]=>
    int(0)
    [1]=>
    int(1)
    [2]=>
    int(2)
  }
  ["obj"]=>
  object(stdClass)#2 (1) {
    ["x"]=>
    array(3) {
      [0]=>
      int(0)
      [1]=>
      int(1)
      [2]=>
      int(2)
    }
  }
  ["ro2"]=>
  int(1)
}
