--TEST--
Native array_key_exists() through the element probes
--DESCRIPTION--
array_key_exists() of literal and CV keys in array CVs, references and
temporaries inline, null elements included; numeric strings, runtime
built keys, floats, bools, null, illegal keys, non-arrays and $GLOBALS take
the helper.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function arr() { return ['t' => null, 5 => 1]; }
function keys($map, $key, $i)
{
    $alias = $map; $ref = &$alias; $runtime = implode('', ['na', 'me']);
    $out = [
        array_key_exists('name', $map), array_key_exists('nil', $map), array_key_exists('zz', $map),
        array_key_exists($key, $map), array_key_exists($runtime, $map), array_key_exists(10, $map),
        array_key_exists('10', $map), array_key_exists(-1, $map), array_key_exists('name', $ref),
        array_key_exists('t', arr()), array_key_exists(5, arr()), array_key_exists('x', arr()),
        array_key_exists($i, [1, 2]), array_key_exists(1.5, [0, 1]), array_key_exists(true, [0, 1]),
    ];
    if (array_key_exists('name', $map)) { $out[] = 'has'; }
    try { $out[] = array_key_exists('a', 'str'); } catch (TypeError $e) { $out[] = 'TypeError'; }
    try { $out[] = array_key_exists([], $map); } catch (TypeError $e) { $out[] = 'TypeError2'; }
    $out[] = array_key_exists(null, ['' => 1]);
    $out[] = array_key_exists('gv', $GLOBALS);
    return $out;
}
$gv = 1;
$map = ['name' => 'n', 'nil' => null, 10 => 'ten', -1 => 'm'];
for ($i = 0; $i < 3; $i++) { echo json_encode(keys($map, $i ? 'nil' : 'none', $i)), "\n"; }
?>
--EXPECTF--

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line %d

Deprecated: Using null as the key parameter for array_key_exists() is deprecated, use an empty string instead in %s on line %d
[true,true,false,false,true,true,true,true,true,true,true,false,true,true,true,"has","TypeError","TypeError2",true,true]

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line %d

Deprecated: Using null as the key parameter for array_key_exists() is deprecated, use an empty string instead in %s on line %d
[true,true,false,true,true,true,true,true,true,true,true,false,true,true,true,"has","TypeError","TypeError2",true,true]

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line %d

Deprecated: Using null as the key parameter for array_key_exists() is deprecated, use an empty string instead in %s on line %d
[true,true,false,true,true,true,true,true,true,true,true,false,false,true,true,"has","TypeError","TypeError2",true,true]
