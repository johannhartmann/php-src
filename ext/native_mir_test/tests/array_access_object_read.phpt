--TEST--
Native: dimension reads of ArrayAccess and ArrayObject objects
--DESCRIPTION--
Reading $cv[$key] of an object goes through its read_dimension handler
without the general fetch: offsetGet() with literal, integer and string
keys, by-reference offsetGet(), exceptions, and ArrayObject warnings.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
namespace NativeArrayAccessRead;

class Box implements \ArrayAccess {
    public array $log = [];
    function __construct(private array $data) {}
    function offsetExists($o): bool { return isset($this->data[$o]); }
    function offsetGet($o): mixed {
        $this->log[] = $o;
        if ($o === 'boom') { throw new \RuntimeException("no $o"); }
        return $this->data[$o] ?? null;
    }
    function offsetSet($o, $v): void { $this->data[$o] = $v; }
    function offsetUnset($o): void { unset($this->data[$o]); }
}
class RefBox extends Box {
    private $value = 5;
    function &offsetGet($o): mixed { return $this->value; }
}

function read(\ArrayAccess $box, $key) { return $box[$key]; }
function readConst(\ArrayAccess $box) { return $box['a']; }
function readNum(\ArrayAccess $box) { return $box[1]; }

$box = new Box(['a' => 1, 1 => 'one', '2' => 'two']);
$array = new \ArrayObject(['a' => 'ao']);
for ($i = 0; $i < 3; $i++) {
    var_dump(read($box, 'a'), readConst($box), readNum($box), read($box, '2'),
        read($box, 'missing'), readConst($array), read(new RefBox([]), 'x'));
}
echo implode(',', $box->log), "\n";
try {
    read($box, 'boom');
} catch (\RuntimeException $e) {
    echo $e->getMessage(), ' at line ', $e->getLine(), "\n";
}
try {
    readConst(new \ArrayObject([]));
} catch (\Throwable $e) {
    echo get_class($e), "\n";
}
var_dump(readConst(new \ArrayObject([])));
?>
--EXPECTF--
int(1)
int(1)
string(3) "one"
string(3) "two"
NULL
string(2) "ao"
int(5)
int(1)
int(1)
string(3) "one"
string(3) "two"
NULL
string(2) "ao"
int(5)
int(1)
int(1)
string(3) "one"
string(3) "two"
NULL
string(2) "ao"
int(5)
a,a,1,2,missing,a,a,1,2,missing,a,a,1,2,missing
no boom at line 10

Warning: Undefined array key "a" in %s on line 22

Warning: Undefined array key "a" in %s on line 22
NULL
