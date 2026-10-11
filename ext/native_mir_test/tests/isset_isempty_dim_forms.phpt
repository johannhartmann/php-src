--TEST--
Native isset() and empty() of dimensions
--DESCRIPTION--
isset() and empty() of array elements under integer, string, numeric-string and other keys, of strings, null, undefined variables, references, temporaries and ArrayAccess objects answer as ZEND_ISSET_ISEMPTY_DIM_OBJ does.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class AA implements ArrayAccess {
    public function offsetExists($o): bool { echo "exists($o)|"; return $o === 'k'; }
    public function offsetGet($o): mixed { echo "get($o)|"; return $o === 'k' ? 0 : null; }
    public function offsetSet($o, $v): void {}
    public function offsetUnset($o): void {}
}
function k($v) { return $v; }
function run($i) {
    $a = ['a' => 1, 'z' => 0, 'n' => null, 5 => '', '7' => [1], 'e' => [], 'o' => new stdClass];
    $r = &$a['a'];
    $nul = null;
    $s = 'string';
    $refnull = null;
    $b = ['rn' => &$refnull];
    $out = [
        isset($a['a']), empty($a['a']), isset($a['z']), empty($a['z']), isset($a['n']), empty($a['n']),
        isset($a[5]), empty($a[5]), isset($a['7']), isset($a[7]), empty($a['e']), empty($a['o']),
        isset($a['missing']), empty($a['missing']), isset($a[k('a')]), empty($a[k('z')]),
        isset($a[1.5]), isset($a[true]), isset($a[null]), isset($nul['x']), empty($nul['x']),
        isset($s[1]), isset($s['1']), isset($s['x']), empty($s[0]), isset($s[10]),
        isset(k($a)['a']), empty(k($a)['z']), isset($b['rn']), empty($b['rn']),
        isset($a['a'], $a['z']), isset($undef['x']), empty($undef['x']),
    ];
    $aa = new AA;
    $out[] = isset($aa['k']);
    $out[] = empty($aa['k']);
    $out[] = isset($aa['q']);
    $out[] = empty($aa[k('k')]);
    echo json_encode($out), "\n";
}
run(1);
run(2);
--EXPECTF--

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 20

Deprecated: Using null as an array offset is deprecated, use an empty string instead in %s on line 20
exists(k)|exists(k)|get(k)|exists(q)|exists(k)|get(k)|[true,false,true,true,false,true,true,true,true,true,true,false,false,true,true,true,false,false,false,false,true,true,true,false,false,false,true,true,false,true,true,false,true,true,true,false,true]

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 20

Deprecated: Using null as an array offset is deprecated, use an empty string instead in %s on line 20
exists(k)|exists(k)|get(k)|exists(q)|exists(k)|get(k)|[true,false,true,true,false,true,true,true,true,true,true,false,false,true,true,true,false,false,false,false,true,true,true,false,false,false,true,true,false,true,true,false,true,true,true,false,true]
