--TEST--
Native array element reads, isset and empty through EncodeGen snippets
--DESCRIPTION--
Element reads, isset() and empty() of CV, temporary and literal arrays under
literal and CV keys: integer and string keys, numeric-looking and runtime
built strings, missing keys and their warning, references, temporaries
whose last owner is the read, indirect elements of $GLOBALS and objects,
whose empty() needs the helper.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class W14Box { public $list = ['a' => 1, 'b' => '', 'c' => [0], 5 => 'five']; }
class W14Countable implements Countable { public function count(): int { return 0; } }

function w14_temp() { return ['t' => 'temp', 0 => 'zero']; }

function w14_elements(array $map, $key, $box)
{
    $alias = 7;
    $map['ref'] = &$alias;
    $runtime = implode('', ['na', 'me']);
    return [
        $map['name'], $map[$key] ?? 'default', $map[$runtime],
        isset($map['name']), isset($map[$key]), isset($map['nil']),
        isset($map['ref']), $map['ref'], isset($map[$runtime]),
        empty($map['zero']), empty($map['name']), empty($map['empty']),
        empty($map['obj']), empty($map['nil']), empty($map['missing']),
        empty($map['list']), empty($map['nolist']), empty($map['half']),
        isset($map['10']), $map['10'] ?? 'none', isset($map[10]),
        isset($map['-1']), $map[-1] ?? 'none',
        $box->list['a'], isset($box->list['b']), empty($box->list['b']),
        $box->list[5], isset($box->list['zz']), empty($box->list['c']),
        w14_temp()['t'], isset(w14_temp()['zero']), empty(w14_temp()[0]),
        ['k' => 'literal']['k'],
    ];
}

$map = [
    'name' => 'native', 'nil' => null, 'zero' => '0', 'empty' => '',
    'obj' => new W14Countable, 'list' => [1], 'nolist' => [], 'half' => 0.0,
    10 => 'ten', -1 => 'minus',
];
for ($i = 0; $i < 3; $i++) {
    echo json_encode(w14_elements($map, $i ? 'name' : 'zz', new W14Box)), "\n";
}
echo @$map['missing'], "|\n";
echo $map['missing'] ?? 'coalesced', "\n";
function w14_missing($map) { return $map['missing']; }
var_dump(w14_missing($map));

$GLOBALS['w14_global'] = 'global';
function w14_globals() { return [$GLOBALS['w14_global'], isset($GLOBALS['w14_global']), empty($GLOBALS['w14_nope'])]; }
echo json_encode(w14_globals()), "\n";

/* Keys sharing buckets: many keys in a small table force collisions. */
$many = [];
for ($i = 0; $i < 200; $i++) {
    $many['key' . $i] = $i;
}
function w14_many($many) {
    $sum = 0;
    foreach (['key0', 'key77', 'key199', 'key200'] as $key) {
        $sum += $many[$key] ?? 1000;
        $sum += isset($many[$key]) ? 1 : 0;
    }
    return $sum + $many['key150'];
}
echo w14_many($many), "\n";
?>
--EXPECTF--
["native","default","native",true,false,false,true,7,true,true,false,true,false,true,true,false,true,true,true,"ten",true,true,"minus",1,true,true,"five",false,false,"temp",false,false,"literal"]
["native","native","native",true,true,false,true,7,true,true,false,true,false,true,true,false,true,true,true,"ten",true,true,"minus",1,true,true,"five",false,false,"temp",false,false,"literal"]
["native","native","native",true,true,false,true,7,true,true,false,true,false,true,true,false,true,true,true,"ten",true,true,"minus",1,true,true,"five",false,false,"temp",false,false,"literal"]
|
coalesced

Warning: Undefined array key "missing" in %s on line %d
NULL
["global",true,true]
1429
