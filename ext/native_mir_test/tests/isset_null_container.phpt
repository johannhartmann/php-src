--TEST--
Inline isset, empty and ?? of an element of a null or undefined container
--DESCRIPTION--
The test probes answer a null, undefined or referenced-null container
without the helper and without a notice; an undefined key, reading such an
element and array_key_exists() on null still warn or throw.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class C { public $cache = null; public $map = ['a' => 1];
    function has($k) { return [isset($this->cache[$k]), empty($this->cache[$k]), $this->cache[$k] ?? 'd', isset($this->map[$k])]; } }
function f($k) {
    $n = null; $r = null; $ref = &$r;
    return [isset($n[$k]), empty($n[$k]), $n[$k] ?? 'n', isset($undef[$k]), $undef[$k] ?? 'u',
        isset($ref[$k]), $ref['x'] ?? 'r', isset($n['lit']), empty($n[0])];
}
$c = new C;
for ($i = 0; $i < 3; $i++) { echo json_encode([f('a'), f(1), $c->has('a'), $c->has('b')]), "\n"; }
$n = null;
echo var_export($n['k'], true), "\n";
var_dump(isset($n[$undefinedKey]));
try { array_key_exists('k', $n); } catch (TypeError $e) { echo get_class($e), "\n"; }
?>
--EXPECTF--
[[false,true,"n",false,"u",false,"r",false,true],[false,true,"n",false,"u",false,"r",false,true],[false,true,"d",true],[false,true,"d",false]]
[[false,true,"n",false,"u",false,"r",false,true],[false,true,"n",false,"u",false,"r",false,true],[false,true,"d",true],[false,true,"d",false]]
[[false,true,"n",false,"u",false,"r",false,true],[false,true,"n",false,"u",false,"r",false,true],[false,true,"d",true],[false,true,"d",false]]

Warning: Trying to access array offset on null in %s on line 12
NULL

Warning: Undefined variable $undefinedKey in %s on line 13
bool(false)
TypeError
