--TEST--
Native isset/empty of array elements with precomputed offsets
--DESCRIPTION--
isset() and empty() of an element of a CV or temporary array under a literal,
CV or temporary key: numeric string keys, references, null and falsy values,
and the generic fallback for objects, strings and other keys.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class W14Holder { public $data; public function __construct($d) { $this->data = $d; } }
class W14Access implements ArrayAccess {
    public function offsetExists($o): bool { return $o === 'yes'; }
    public function offsetGet($o): mixed { return $o === 'yes' ? '0' : null; }
    public function offsetSet($o, $v): void {}
    public function offsetUnset($o): void {}
}
function w14_probe($array, $key, $holder)
{
    $ref = null;
    $array['ref'] = &$ref;
    return [
        isset($array['a']), empty($array['a']),
        isset($array['1']), isset($array[1]), isset($array[$key]), empty($array[$key]),
        isset($array['n']), empty($array['z']), isset($array['ref']), empty($array['missing']),
        isset($array[$key . '']), isset($holder->data['a']), empty($holder->data['z']),
        isset($holder->data[$key]),
    ];
}
$data = ['a' => 'x', 1 => 'one', 'n' => null, 'z' => '0', 'k' => [1]];
for ($i = 0; $i < 2; $i++) {
    echo json_encode(w14_probe($data, 'k', new W14Holder($data))), "\n";
    echo json_encode(w14_probe($data, 'missing', new W14Holder($data))), "\n";
}
$object = new W14Access;
$string = 'abc';
var_dump(isset($object['yes']), empty($object['yes']), isset($string[1]), isset($string['x']), empty($string[5]));
?>
--EXPECT--
[true,false,true,true,true,false,false,true,false,true,true,true,true,true]
[true,false,true,true,false,true,false,true,false,true,false,true,true,false]
[true,false,true,true,true,false,false,true,false,true,true,true,true,true]
[true,false,true,true,false,true,false,true,false,true,false,true,true,false]
bool(true)
bool(true)
bool(true)
bool(false)
bool(true)
