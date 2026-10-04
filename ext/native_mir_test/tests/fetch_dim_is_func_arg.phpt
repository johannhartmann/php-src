--TEST--
Native FETCH_DIM_IS and by-value FETCH_DIM_FUNC_ARG fast paths
--DESCRIPTION--
?? on an array or null reads integer and string keys without the generic
fetch, as zend_fetch_dimension_address_read_IS() does, and a dimension
passed to a by-value parameter reads like FETCH_DIM_R. Missing and null
elements, numeric string keys, references, temporaries, strings,
ArrayAccess, null and float keys and by-reference parameters keep stock
semantics.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function f() { return ['a' => 1, 'n' => null, '5' => 'five', 'r' => [1, 2]]; }
function byref(&$x) { $x = 'set'; }
function byval($x) { return $x; }
class AA implements ArrayAccess { function offsetExists($o): bool { return $o === 'k'; } function offsetGet($o): mixed { return "got$o"; } function offsetSet($o, $v): void {} function offsetUnset($o): void {} }
function t($r) {
  $a = f(); $ref = &$a['a']; $s = 'str'; $o = new AA; $n = null;
  $out = [$a['a'] ?? 'd', $a['x'] ?? 'd', $a['n'] ?? 'd', $a[5] ?? 'd', $a['5'] ?? 'd', f()['r'][1] ?? 'd', f()['zz'] ?? 'd',
    $n['x'] ?? 'd', $undef['x'] ?? 'd', $s[0] ?? 'd', $s['x'] ?? 'd', $o['k'] ?? 'd', $o['q'] ?? 'd', $a[null] ?? 'd', $a[1.5] ?? 'd',
    isset($a['r'][0]), byval($a['a']), byval(f()['r'][0])];
  byref($a['new']); $out[] = $a['new'];
  byref($a['r'][5]); $out[] = $a['r'][5];
  return $out;
}
for ($i = 0; $i < 2; $i++) echo json_encode(t($i)), "\n";
?>
--EXPECTF--

Deprecated: Using null as an array offset is deprecated, use an empty string instead in %s on line %d

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line %d
[1,"d","d","five","five",2,"d","d","d","s","d","gotk","d","d","d",true,1,1,"set","set"]

Deprecated: Using null as an array offset is deprecated, use an empty string instead in %s on line %d

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line %d
[1,"d","d","five","five",2,"d","d","d","s","d","gotk","d","d","d",true,1,1,"set","set"]
