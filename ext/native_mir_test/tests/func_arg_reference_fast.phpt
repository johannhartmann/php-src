--TEST--
Native: properties and elements passed by reference through the fast call path
--DESCRIPTION--
A namespaced call cannot tell at compile time whether a property or element
argument goes by reference (CHECK_FUNC_ARG, a FUNC_ARG fetch, SEND_FUNC_ARG).
A published site passes it by reference exactly to by-reference parameters
of internal and user targets: end(), array_pop() and sort() on properties,
typed and untyped, array elements and static properties, mixed with
by-value arguments, magic properties that fetch a temporary, and CVs sent
with SEND_REF, also undefined ones.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
namespace NativeFuncArgRef;

function push(array &$list, $value) { $list[] = $value; return count($list); }
function first($value, array &$list) { return array_shift($list) . $value; }
function add(&$list, $value) { $list[] = $value; return count($list); }

class Stack {
    public $items = [];
    private array $typed = [];
    public static $shared = [];
    private $data = ['a' => [1, 2, 3]];

    public function __get($name) { return [7, 8, 9]; }

    public function run(int $i) {
        $this->items = [1, 2, 3 + $i];
        $this->typed = [4, 5, 6 + $i];
        $out = [end($this->items), end($this->typed), array_pop($this->typed), count($this->typed)];
        $out[] = current($this->items);
        reset($this->items);
        $out[] = current($this->items);
        sort($this->data['a']);
        $out[] = end($this->data['a']);
        $out[] = push($this->items, $i);
        $out[] = first('x', $this->items);
        $out[] = push(self::$shared, $i);
        $out[] = end($this->magic);
        $out[] = implode(',', $this->items);
        $out[] = str_repeat('y', count($this->items));
        $out[] = add($fresh, $i) . ':' . implode(',', $fresh);
        $list = [$i, 5];
        $out[] = first('z', $list) . ':' . implode(',', $list);
        return implode(' ', $out);
    }
}

$s = new Stack;
for ($i = 0; $i < 3; $i++) {
    echo $s->run($i), "\n";
}
?>
--EXPECTF--
Notice: Indirect modification of overloaded property NativeFuncArgRef\Stack::$magic has no effect in %s on line %d
3 6 6 2 3 1 3 4 1x 1 9 2,3,0 yyy 1:0 0z:5

Notice: Indirect modification of overloaded property NativeFuncArgRef\Stack::$magic has no effect in %s on line %d
4 7 7 2 4 1 3 4 1x 2 9 2,4,1 yyy 1:1 1z:5

Notice: Indirect modification of overloaded property NativeFuncArgRef\Stack::$magic has no effect in %s on line %d
5 8 8 2 5 1 3 4 1x 3 9 2,5,2 yyy 1:2 2z:5
