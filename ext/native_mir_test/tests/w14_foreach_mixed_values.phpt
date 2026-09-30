--TEST--
Native by-value foreach over arrays of mixed values
--DESCRIPTION--
FE_FETCH_R copies any element that is no reference into a CV owning nothing
counted: strings, arrays, objects, null, floats, with and without keys and
over holes. A reference element, a CV that owns a counted value and a
by-reference loop keep the helper; copies stay shared until written and the
last owner still destroys objects.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class W14Item {
    public function __construct(public $name) {}
    public function __destruct() { echo "destroy {$this->name}\n"; }
}

function w14_values(array $values)
{
    $out = [];
    foreach ($values as $key => $value) {
        $out[] = gettype($key) . ':' . $key . '=' . json_encode($value);
    }
    return implode(' ', $out);
}

function w14_counted(array $values)
{
    $value = [1, 2];
    $seen = [];
    foreach ($values as $value) {
        $seen[] = is_array($value) ? count($value) : $value;
    }
    return implode(',', $seen) . '|' . json_encode($value);
}

function w14_shared(array $rows)
{
    $copies = [];
    foreach ($rows as $row) {
        $copies[] = $row;
    }
    $copies[0][] = 'changed';
    return json_encode([$rows[0], $copies[0], $copies[1]]);
}

function w14_objects()
{
    $items = [new W14Item('a'), new W14Item('b')];
    foreach ($items as $item) {
        echo "visit {$item->name}\n";
    }
    unset($items);
    echo "after unset\n";
    unset($item);
    echo "end\n";
}

$prefix = str_repeat('k', 2);
$map = [
    'int' => 1, 'str' => 'two', 'arr' => [1, [2]], 'null' => null,
    'float' => 1.5, 'bool' => false, $prefix . 'dyn' => $prefix . 'val', 7 => 'seven',
];
for ($i = 0; $i < 2; $i++) {
    echo w14_values($map), "\n";
}
$holes = [1 => 'a', 2 => 'b', 3 => 'c', 4 => 'd'];
unset($holes[2]);
echo w14_values($holes), "\n";
$packed = ['x', ['y'], null, 3];
echo w14_values($packed), "\n";

$target = 5;
$with_reference = ['a' => 1, 'r' => &$target, 'z' => 'end'];
echo w14_values($with_reference), "\n";
$target = 6;
echo w14_values($with_reference), "\n";

echo w14_counted([[1], 'b', [1, 2, 3]]), "\n";
echo w14_shared([['r0'], ['r1']]), "\n";
w14_objects();

$by_reference = ['a' => 1, 'b' => 2];
foreach ($by_reference as &$slot) {
    $slot *= 10;
}
unset($slot);
echo json_encode($by_reference), "\n";
?>
--EXPECT--
string:int=1 string:str="two" string:arr=[1,[2]] string:null=null string:float=1.5 string:bool=false string:kkdyn="kkval" integer:7="seven"
string:int=1 string:str="two" string:arr=[1,[2]] string:null=null string:float=1.5 string:bool=false string:kkdyn="kkval" integer:7="seven"
integer:1="a" integer:3="c" integer:4="d"
integer:0="x" integer:1=["y"] integer:2=null integer:3=3
string:a=1 string:r=5 string:z="end"
string:a=1 string:r=6 string:z="end"
1,b,3|[1,2,3]
[["r0"],["r0","changed"],["r1"]]
visit a
visit b
destroy a
after unset
destroy b
end
{"a":10,"b":20}
