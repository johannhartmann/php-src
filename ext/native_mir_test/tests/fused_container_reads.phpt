--TEST--
Native reads fused into the array read of their value: property, element and chained containers, warnings, error handlers and references
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class Holder implements ArrayAccess {
    public $opts = ['a' => ['b' => 'deep', 'n' => null], 'k' => 'v', 1 => 'one', 'n' => null];
    public $ref;
    public $s = 'hello';
    public $none = null;
    public $obj;
    public array $typed;
    public int $num = 5;
    private $hidden = ['h' => 1];
    function __construct() {
        $x = ['r' => 'viaref'];
        $this->ref = &$x;
        $this->obj = $this;
    }
    function offsetExists($o): bool { echo "exists($o)\n"; return $o === 'aa'; }
    function offsetGet($o): mixed { echo "get($o)\n"; return ['inner' => "got$o"]; }
    function offsetSet($o, $v): void {}
    function offsetUnset($o): void {}
    function __get($n) { echo "__get($n)\n"; return ['magic' => $n]; }
    function reads($k, $j) {
        $r = [];
        $r[] = $this->opts['k'];
        $r[] = $this->opts[1];
        $r[] = $this->opts[$k];
        $r[] = $this->opts['a']['b'];
        $r[] = $this->opts[$j]['b'];
        $r[] = $this->ref['r'];
        $r[] = $this->s[1];
        $r[] = $this->s[$k === 'k' ? 0 : 1];
        $r[] = $this->opts['zz'] ?? 'dflt';
        $r[] = $this->opts['a']['zz'] ?? 'dflt2';
        $r[] = $this->opts['zz']['yy'] ?? 'dflt3';
        $r[] = $this->none['x'] ?? 'dflt4';
        $r[] = $this->opts['n']['x'] ?? 'dflt5';
        $r[] = isset($this->opts['k']) ? 'set' : 'unset';
        $r[] = isset($this->opts['n']) ? 'set' : 'unset';
        $r[] = isset($this->opts['a']['b']) ? 'set' : 'unset';
        $r[] = isset($this->opts['zz']['b']) ? 'set' : 'unset';
        $r[] = empty($this->opts['k']) ? 'empty' : 'full';
        $r[] = empty($this->opts['a']['n']) ? 'empty' : 'full';
        $r[] = $this->hidden['h'];
        $r[] = $this->magic['magic'];
        $r[] = $this->obj['aa']['inner'];
        $r[] = isset($this->obj['aa']) ? 'oset' : 'ounset';
        $r[] = isset($this->obj['bb']) ? 'oset' : 'ounset';
        $r[] = $this->opts['a']['b'][0];
        return $r;
    }
    function warn() {
        $r = [];
        $r[] = $this->opts['missing'];
        $r[] = $this->opts['missing']['deeper'];
        $r[] = $this->opts['a']['missing'];
        $r[] = $this->none['x'];
        $r[] = $this->num['x'];
        $r[] = $this->opts['k']['x'] ?? 'strdflt';
        return $r;
    }
    function typed() {
        try { return $this->typed['x']; } catch (Error $e) { return get_class($e) . ': ' . $e->getMessage(); }
    }
    function undefinedKey() {
        $r = [];
        $r[] = $this->none[$undef] ?? 'u1';
        $r[] = $this->opts['zz'][$undef2] ?? 'u2';
        $r[] = isset($this->opts['zz'][$undef3]) ? 'set' : 'u3';
        $r[] = $this->opts[$undef4]['b'] ?? 'u4';
        return $r;
    }
}

function chains(array $a, $k) {
    $r = [];
    $r[] = $a['x']['y'];
    $r[] = $a['x']['y']['z'] ?? 'nz';
    $r[] = $a['x'][$k] ?? 'nk';
    $r[] = $a[$k]['y'] ?? 'nk2';
    $r[] = isset($a['x']['y']) ? 'iy' : 'ny';
    $r[] = isset($a['q']['y']) ? 'iq' : 'nq';
    $r[] = empty($a['x']['e']) ? 'ee' : 'ne';
    $r[] = $a['l'][0][1];
    $r[] = $a['l'][1][0] ?? 'nl';
    $r[] = $a['r']['v'];
    return $r;
}

function heldValue(Holder $h) {
    $v = $h->opts['a'];
    $h->opts['a'] = 'replaced';
    return [$v['b'], $h->opts['a']];
}

$h = new Holder;
var_dump($h->reads('k', 'a'));
var_dump($h->warn());
var_dump($h->typed());
var_dump($h->undefinedKey());
$inner = ['v' => 'refd'];
$arr = ['x' => ['y' => 'xy', 'e' => 0], 'l' => [[0, 'l01']], 'r' => &$inner];
var_dump(chains($arr, 'y'));
var_dump(chains($arr, 'nope'));
var_dump(heldValue(new Holder));

/* An error handler that replaces the container while the read warns. */
class Mut {
    public $data = ['a' => ['b' => 1]];
    function run() {
        set_error_handler(function ($no, $str) {
            echo "handler: $str\n";
            $this->data = str_repeat('z', 3);
            return true;
        });
        $x = $this->data['a']['missing'];
        var_dump($x, $this->data);
        $this->data = ['a' => ['b' => 1]];
        $y = $this->data['nope'];
        var_dump($y, $this->data);
        restore_error_handler();
        set_error_handler(function ($no, $str) { throw new Exception("thrown: $str"); });
        $this->data = ['a' => ['b' => 1]];
        try { $z = $this->data['a']['missing']; } catch (Exception $e) { echo $e->getMessage(), "\n"; }
        try { $z = $this->data['gone']['b']; } catch (Exception $e) { echo $e->getMessage(), "\n"; }
        restore_error_handler();
    }
}
(new Mut)->run();

/* Many iterations: counted values keep their counts. */
function loop(Holder $h) {
    $out = 0;
    for ($i = 0; $i < 2000; $i++) {
        $out += strlen($h->opts['a']['b']) + strlen($h->opts['k'] ?? '') + (isset($h->opts['a']['n']) ? 1 : 0);
        $key = str_repeat('k', 1);
        $out += strlen($h->opts[$key]);
    }
    return $out;
}
var_dump(loop(new Holder));

/* Longer chains, string containers and slots OPcache reuses along a chain. */
function deep(array $a, $k) {
    $r = [];
    $r[] = $a['p']['q']['r']['s'];
    $r[] = $a['p']['q'][$k]['s'] ?? 'nd';
    $r[] = isset($a['p']['q']['r']['s']) ? 'i' : 'n';
    $r[] = isset($a['p']['x']['r']['s']) ? 'i' : 'n';
    $r[] = empty($a['p']['q']['r']['z']) ? 'e' : 'f';
    $r[] = $a['str']['x'] ?? 'sdflt';
    $r[] = isset($a['str'][1]) ? 'so' : 'sn';
    $r[] = $a['str'][2];
    $r[] = $a['int']['x'] ?? 'idflt';
    return $r;
}
$deep = ['p' => ['q' => ['r' => ['s' => 'S', 'z' => 0]]], 'str' => 'abc', 'int' => 7];
for ($i = 0; $i < 3; $i++) {
    var_dump(deep($deep, $i ? 'r' : 'zz'));
}
class Typed {
    public array $list = ['a' => [1, 2]];
    public ?array $maybe = null;
    function get() { return [$this->list['a'][1], $this->maybe['x'] ?? 'mnull', isset($this->list['a'][5]) ? 1 : 0]; }
}
var_dump((new Typed)->get());

/* OPcache reuses temporaries along a chain: a failure past the second
 * level must run the reads from there. */
function aliasRead(array $a) {
    return $a['p']['q']['r']['s'];
}
function aliasCoalesce(array $a, $k) {
    return $a['p']['q'][$k]['s'] ?? 'dflt';
}
$ok = ['p' => ['q' => ['r' => ['s' => 'S']]]];
$noR = ['p' => ['q' => ['x' => 1]], 's' => 'WRONG'];
$strR = ['p' => ['q' => ['r' => 'str']]];
var_dump(aliasRead($ok), aliasRead($noR));
try {
    var_dump(aliasRead($strR));
} catch (TypeError $e) {
    echo get_class($e), ': ', $e->getMessage(), "\n";
}
var_dump(aliasCoalesce($ok, 'r'), aliasCoalesce($noR, 'r'));
var_dump(aliasCoalesce(['p' => ['q' => ['r' => new ArrayObject(['s' => 'viaobj'])]]], 'r'));
?>
--EXPECTF--
__get(magic)
get(aa)
exists(aa)
exists(bb)
array(25) {
  [0]=>
  string(1) "v"
  [1]=>
  string(3) "one"
  [2]=>
  string(1) "v"
  [3]=>
  string(4) "deep"
  [4]=>
  string(4) "deep"
  [5]=>
  string(6) "viaref"
  [6]=>
  string(1) "e"
  [7]=>
  string(1) "h"
  [8]=>
  string(4) "dflt"
  [9]=>
  string(5) "dflt2"
  [10]=>
  string(5) "dflt3"
  [11]=>
  string(5) "dflt4"
  [12]=>
  string(5) "dflt5"
  [13]=>
  string(3) "set"
  [14]=>
  string(5) "unset"
  [15]=>
  string(3) "set"
  [16]=>
  string(5) "unset"
  [17]=>
  string(4) "full"
  [18]=>
  string(5) "empty"
  [19]=>
  int(1)
  [20]=>
  string(5) "magic"
  [21]=>
  string(5) "gotaa"
  [22]=>
  string(4) "oset"
  [23]=>
  string(6) "ounset"
  [24]=>
  string(1) "d"
}

Warning: Undefined array key "missing" in %s on line 52

Warning: Undefined array key "missing" in %s on line 53

Warning: Trying to access array offset on null in %s on line 53

Warning: Undefined array key "missing" in %s on line 54

Warning: Trying to access array offset on null in %s on line 55

Warning: Trying to access array offset on int in %s on line 56
array(6) {
  [0]=>
  NULL
  [1]=>
  NULL
  [2]=>
  NULL
  [3]=>
  NULL
  [4]=>
  NULL
  [5]=>
  string(7) "strdflt"
}
string(79) "Error: Typed property Holder::$typed must not be accessed before initialization"

Warning: Undefined variable $undef in %s on line 65

Warning: Undefined variable $undef2 in %s on line 66

Warning: Undefined variable $undef3 in %s on line 67

Warning: Undefined variable $undef4 in %s on line 68

Deprecated: Using null as an array offset is deprecated, use an empty string instead in %s on line 68
array(4) {
  [0]=>
  string(2) "u1"
  [1]=>
  string(2) "u2"
  [2]=>
  string(2) "u3"
  [3]=>
  string(2) "u4"
}
array(10) {
  [0]=>
  string(2) "xy"
  [1]=>
  string(2) "nz"
  [2]=>
  string(2) "xy"
  [3]=>
  string(3) "nk2"
  [4]=>
  string(2) "iy"
  [5]=>
  string(2) "nq"
  [6]=>
  string(2) "ee"
  [7]=>
  string(3) "l01"
  [8]=>
  string(2) "nl"
  [9]=>
  string(4) "refd"
}
array(10) {
  [0]=>
  string(2) "xy"
  [1]=>
  string(2) "nz"
  [2]=>
  string(2) "nk"
  [3]=>
  string(3) "nk2"
  [4]=>
  string(2) "iy"
  [5]=>
  string(2) "nq"
  [6]=>
  string(2) "ee"
  [7]=>
  string(3) "l01"
  [8]=>
  string(2) "nl"
  [9]=>
  string(4) "refd"
}
array(2) {
  [0]=>
  string(4) "deep"
  [1]=>
  string(8) "replaced"
}
handler: Undefined array key "missing"
NULL
string(3) "zzz"
handler: Undefined array key "nope"
NULL
string(3) "zzz"
thrown: Undefined array key "missing"
thrown: Undefined array key "gone"
int(12000)
array(9) {
  [0]=>
  string(1) "S"
  [1]=>
  string(2) "nd"
  [2]=>
  string(1) "i"
  [3]=>
  string(1) "n"
  [4]=>
  string(1) "e"
  [5]=>
  string(5) "sdflt"
  [6]=>
  string(2) "so"
  [7]=>
  string(1) "c"
  [8]=>
  string(5) "idflt"
}
array(9) {
  [0]=>
  string(1) "S"
  [1]=>
  string(1) "S"
  [2]=>
  string(1) "i"
  [3]=>
  string(1) "n"
  [4]=>
  string(1) "e"
  [5]=>
  string(5) "sdflt"
  [6]=>
  string(2) "so"
  [7]=>
  string(1) "c"
  [8]=>
  string(5) "idflt"
}
array(9) {
  [0]=>
  string(1) "S"
  [1]=>
  string(1) "S"
  [2]=>
  string(1) "i"
  [3]=>
  string(1) "n"
  [4]=>
  string(1) "e"
  [5]=>
  string(5) "sdflt"
  [6]=>
  string(2) "so"
  [7]=>
  string(1) "c"
  [8]=>
  string(5) "idflt"
}
array(3) {
  [0]=>
  int(2)
  [1]=>
  string(5) "mnull"
  [2]=>
  int(0)
}

Warning: Undefined array key "r" in %s on line 169

Warning: Trying to access array offset on null in %s on line 169
string(1) "S"
NULL
TypeError: Cannot access offset of type string on string
string(1) "S"
string(4) "dflt"
string(6) "viaobj"
