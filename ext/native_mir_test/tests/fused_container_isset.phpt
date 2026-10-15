--TEST--
Native isset() and empty() of fused containers in conditions: properties, chains, ArrayAccess, strings, undefined keys and error handlers
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class Conf implements ArrayAccess {
    public $opts = ['a' => ['b' => 1, 'n' => null], 'k' => 'v', 's' => 'str'];
    public $ref;
    public $none = null;
    public $obj;
    function __construct() { $x = ['r' => ['q' => 1]]; $this->ref = &$x; $this->obj = $this; }
    function offsetExists($o): bool { echo "exists($o)\n"; return $o === 'yes'; }
    function offsetGet($o): mixed { echo "get($o)\n"; return ['inner' => 1]; }
    function offsetSet($o, $v): void {}
    function offsetUnset($o): void {}
    function __isset($n) { echo "__isset($n)\n"; return true; }
    function __get($n) { echo "__get($n)\n"; return ['m' => 1]; }
    function branches($k, $j) {
        $r = '';
        if (isset($this->opts['a']['b'])) { $r .= '1'; }
        if (isset($this->opts['a']['n'])) { $r .= 'X'; } else { $r .= '2'; }
        if (!isset($this->opts['z']['b'])) { $r .= '3'; }
        if (isset($this->opts[$k][$j])) { $r .= '4'; }
        if (isset($this->opts['k'])) { $r .= '5'; }
        if (isset($this->ref['r']['q'])) { $r .= '6'; }
        if (isset($this->none['x'])) { $r .= 'X'; } else { $r .= '7'; }
        if (isset($this->opts['s'][1])) { $r .= '8'; }
        if (isset($this->opts['s']['x'])) { $r .= 'X'; } else { $r .= '9'; }
        if (isset($this->obj['yes'])) { $r .= 'A'; }
        if (isset($this->obj['no'])) { $r .= 'X'; } else { $r .= 'B'; }
        if (isset($this->magic['m'])) { $r .= 'C'; }
        if (isset($this->opts['a']['b']) && isset($this->opts['k'])) { $r .= 'D'; }
        if (isset($this->opts['a'][$undefKey])) { $r .= 'X'; } else { $r .= 'E'; }
        if (isset($this->none[$undefKey2])) { $r .= 'X'; } else { $r .= 'F'; }
        return $r;
    }
}
function chains(array $a, $k) {
    $r = '';
    for ($i = 0; $i < 3; $i++) {
        if (isset($a['p']['q']['r']['s'])) { $r .= 'a'; }
        if (isset($a['p']['q'][$k]['s'])) { $r .= 'b'; } else { $r .= 'c'; }
        if (isset($a['p']['x']['r']['s'])) { $r .= 'X'; } else { $r .= 'd'; }
        if (isset($a['str']['x']['y'])) { $r .= 'X'; } else { $r .= 'e'; }
        if (isset($a['ref']['v']['w'])) { $r .= 'f'; }
    }
    return $r;
}
var_dump((new Conf)->branches('a', 'b'));
var_dump((new Conf)->branches('zz', 'b'));
$inner = ['v' => ['w' => 1]];
$a = ['p' => ['q' => ['r' => ['s' => 1]]], 'str' => 'abc', 'ref' => &$inner];
var_dump(chains($a, 'r'), chains($a, 'nope'));
/* An error handler that replaces the container while isset() warns. */
class Mut {
    public $data = ['a' => ['b' => 1]];
    function run() {
        set_error_handler(function ($no, $str) {
            echo "handler: $str\n";
            $this->data = 'gone';
            return true;
        });
        if (isset($this->data['a'][$undef])) { echo "set\n"; } else { echo "unset\n"; }
        var_dump($this->data);
        restore_error_handler();
        $this->data = ['a' => ['b' => 1]];
        set_error_handler(function ($no, $str) { throw new Exception("thrown: $str"); });
        try { if (isset($this->data['a'][$undef2])) { echo "set\n"; } } catch (Exception $e) { echo $e->getMessage(), "\n"; }
        restore_error_handler();
    }
}
(new Mut)->run();
?>
--EXPECTF--
exists(yes)
exists(no)
__isset(magic)
__get(magic)

Warning: Undefined variable $undefKey in %s on line 29

Deprecated: Using null as an array offset is deprecated, use an empty string instead in %s on line 29

Warning: Undefined variable $undefKey2 in %s on line 30
string(15) "123456789ABCDEF"
exists(yes)
exists(no)
__isset(magic)
__get(magic)

Warning: Undefined variable $undefKey in %s on line 29

Deprecated: Using null as an array offset is deprecated, use an empty string instead in %s on line 29

Warning: Undefined variable $undefKey2 in %s on line 30
string(14) "12356789ABCDEF"
string(15) "abdefabdefabdef"
string(15) "acdefacdefacdef"
handler: Undefined variable $undef
handler: Using null as an array offset is deprecated, use an empty string instead
unset
string(4) "gone"
thrown: Undefined variable $undef2
