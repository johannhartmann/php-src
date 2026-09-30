--TEST--
Native property reads, isset and writes through array elements
--DESCRIPTION--
A declared property named by a literal is read, tested and addressed through
the VM run-time cache slot: counted values are shared, element reads and
isset/empty consume the temporary container, and element writes separate a
shared array. Typed, readonly, unset, magic, dynamic and reference-holding
properties and receivers of other classes at the same site keep the general
path.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class W14Bag {
    public $arr = ['k' => 'v', 'n' => null, 5 => [1, 2]];
    public array $typed = ['t' => 1];
    public $plain = 'text';
    public function __construct(public readonly array $fixed = ['r' => 1]) {}
    public function local() { return $this->arr['k'] . '/' . count($this->arr[5]); }
    public function put($key, $value) { $this->arr[$key] = $value; $this->typed[$key] = $value; }
}
class W14Other extends W14Bag {
    public $arr = ['k' => 'other', 5 => [9]];
}
class W14Magic {
    private $data = ['m' => 'magic'];
    public function __get($name) { echo "get $name\n"; return $this->data; }
    public function __isset($name) { echo "isset $name\n"; return true; }
}

function w14_read($bag)
{
    return [
        $bag->arr['k'], $bag->arr[5][1] ?? 'none', isset($bag->arr['k']),
        isset($bag->arr['n']), empty($bag->arr['missing']), $bag->plain,
        $bag->typed['t'] ?? 'unset', $bag->fixed['r'],
    ];
}

function w14_write($bag, $i)
{
    $bag->arr['w' . $i] = $i;
    $bag->arr[5][] = $i;
    $bag->typed['x'] = $i;
    return count($bag->arr) . ':' . count($bag->typed);
}

$bag = new W14Bag;
$other = new W14Other;
for ($i = 0; $i < 3; $i++) {
    echo json_encode(w14_read($i === 1 ? $other : $bag)), "\n";
}
$copy = $bag->arr;
for ($i = 0; $i < 2; $i++) {
    echo w14_write($bag, $i), "\n";
}
echo json_encode($copy), "\n";
echo json_encode($bag->arr), "\n";
echo $bag->local(), ' ', $other->local(), "\n";
$bag->put('p', 'q');
echo $bag->arr['p'], $bag->typed['p'], "\n";

$alias = ['a' => 1];
$bag->arr = &$alias;
$bag->arr['b'] = 2;
echo json_encode($alias), "\n";
unset($bag->arr);
try {
    echo json_encode(w14_read($bag)), "\n";
} catch (Throwable $e) {
    echo get_class($e), "\n";
}
try {
    $bag->fixed['r'] = 2;
} catch (Error $e) {
    echo get_class($e), ': ', $e->getMessage(), "\n";
}
$magic = new W14Magic;
echo $magic->data['m'], "\n";
var_dump(isset($magic->data['m']));
$dynamic = new stdClass;
$dynamic->list = ['d' => 4];
$dynamic->list['e'] = 5;
echo json_encode($dynamic->list), ' ', $dynamic->list['d'], "\n";
?>
--EXPECTF--
["v",2,true,false,true,"text",1,1]
["other","none",true,false,true,"text",1,1]
["v",2,true,false,true,"text",1,1]
4:2
5:2
{"k":"v","n":null,"5":[1,2]}
{"k":"v","n":null,"5":[1,2,0,1],"w0":0,"w1":1}
v/4 other/1
qq
{"a":1,"b":2}

Warning: Undefined property: W14Bag::$arr in %s on line %d

Warning: Trying to access array offset on null in %s on line %d
[null,"none",false,false,true,"text",1,1]
Error: Cannot indirectly modify readonly property W14Bag::$fixed
get data
magic
isset data
get data
bool(true)
{"d":4,"e":5} 4
