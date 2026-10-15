--TEST--
Native array reads of class constants fused into the read: self::, static::, parent::, literal classes, interfaces and scalars
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
interface Keys { const IFACE = ['i' => 'iface']; }
class Base implements Keys {
    const MAP = ['a' => 1, 'b' => ['c' => 'deep'], 'n' => null, 's' => 'str'];
    const SCALAR = 'abc';
    const NUMS = [10, 20, 30];
    function lookups($k, $i) {
        $r = [];
        $r[] = isset(self::MAP[$k]) ? 'set' : 'unset';
        $r[] = isset(static::MAP[$k]) ? 'sset' : 'sunset';
        $r[] = self::MAP['b']['c'];
        $r[] = self::MAP[$k] ?? 'dflt';
        $r[] = static::MAP['b']['zz'] ?? 'dflt2';
        $r[] = self::NUMS[$i] ?? 'nonum';
        $r[] = self::SCALAR[1];
        $r[] = isset(self::SCALAR[5]) ? 'x' : 'noff';
        $r[] = Base::MAP['a'];
        $r[] = self::IFACE['i'];
        $r[] = empty(self::MAP['n']) ? 'empty' : 'full';
        if (isset(self::MAP[$k])) { $r[] = 'branch'; }
        return $r;
    }
    static function st($k) { return [isset(static::MAP[$k]), static::MAP['a'] ?? 0, parent_check::class]; }
    function missing() { return self::MAP['nokey']; }
}
class parent_check {}
class Child extends Base {
    const MAP = ['a' => 'child', 'b' => ['c' => 'childdeep']];
    function viaParent($k) { return [parent::MAP[$k] ?? 'np', self::MAP[$k] ?? 'ns']; }
}
$b = new Base; $c = new Child;
for ($i = 0; $i < 2; $i++) {
    var_dump($b->lookups('a', 1), $b->lookups('zz', 7), $c->lookups('a', 2), $c->lookups('n', 0));
    var_dump(Base::st('a'), Child::st('b'), $c->viaParent('a'), $c->viaParent('s'));
}
var_dump($b->missing());
?>
--EXPECTF--
array(12) {
  [0]=>
  string(3) "set"
  [1]=>
  string(4) "sset"
  [2]=>
  string(4) "deep"
  [3]=>
  int(1)
  [4]=>
  string(5) "dflt2"
  [5]=>
  int(20)
  [6]=>
  string(1) "b"
  [7]=>
  string(4) "noff"
  [8]=>
  int(1)
  [9]=>
  string(5) "iface"
  [10]=>
  string(5) "empty"
  [11]=>
  string(6) "branch"
}
array(11) {
  [0]=>
  string(5) "unset"
  [1]=>
  string(6) "sunset"
  [2]=>
  string(4) "deep"
  [3]=>
  string(4) "dflt"
  [4]=>
  string(5) "dflt2"
  [5]=>
  string(5) "nonum"
  [6]=>
  string(1) "b"
  [7]=>
  string(4) "noff"
  [8]=>
  int(1)
  [9]=>
  string(5) "iface"
  [10]=>
  string(5) "empty"
}
array(12) {
  [0]=>
  string(3) "set"
  [1]=>
  string(4) "sset"
  [2]=>
  string(4) "deep"
  [3]=>
  int(1)
  [4]=>
  string(5) "dflt2"
  [5]=>
  int(30)
  [6]=>
  string(1) "b"
  [7]=>
  string(4) "noff"
  [8]=>
  int(1)
  [9]=>
  string(5) "iface"
  [10]=>
  string(5) "empty"
  [11]=>
  string(6) "branch"
}
array(11) {
  [0]=>
  string(5) "unset"
  [1]=>
  string(6) "sunset"
  [2]=>
  string(4) "deep"
  [3]=>
  string(4) "dflt"
  [4]=>
  string(5) "dflt2"
  [5]=>
  int(10)
  [6]=>
  string(1) "b"
  [7]=>
  string(4) "noff"
  [8]=>
  int(1)
  [9]=>
  string(5) "iface"
  [10]=>
  string(5) "empty"
}
array(3) {
  [0]=>
  bool(true)
  [1]=>
  int(1)
  [2]=>
  string(12) "parent_check"
}
array(3) {
  [0]=>
  bool(true)
  [1]=>
  string(5) "child"
  [2]=>
  string(12) "parent_check"
}
array(2) {
  [0]=>
  int(1)
  [1]=>
  string(5) "child"
}
array(2) {
  [0]=>
  string(3) "str"
  [1]=>
  string(2) "ns"
}
array(12) {
  [0]=>
  string(3) "set"
  [1]=>
  string(4) "sset"
  [2]=>
  string(4) "deep"
  [3]=>
  int(1)
  [4]=>
  string(5) "dflt2"
  [5]=>
  int(20)
  [6]=>
  string(1) "b"
  [7]=>
  string(4) "noff"
  [8]=>
  int(1)
  [9]=>
  string(5) "iface"
  [10]=>
  string(5) "empty"
  [11]=>
  string(6) "branch"
}
array(11) {
  [0]=>
  string(5) "unset"
  [1]=>
  string(6) "sunset"
  [2]=>
  string(4) "deep"
  [3]=>
  string(4) "dflt"
  [4]=>
  string(5) "dflt2"
  [5]=>
  string(5) "nonum"
  [6]=>
  string(1) "b"
  [7]=>
  string(4) "noff"
  [8]=>
  int(1)
  [9]=>
  string(5) "iface"
  [10]=>
  string(5) "empty"
}
array(12) {
  [0]=>
  string(3) "set"
  [1]=>
  string(4) "sset"
  [2]=>
  string(4) "deep"
  [3]=>
  int(1)
  [4]=>
  string(5) "dflt2"
  [5]=>
  int(30)
  [6]=>
  string(1) "b"
  [7]=>
  string(4) "noff"
  [8]=>
  int(1)
  [9]=>
  string(5) "iface"
  [10]=>
  string(5) "empty"
  [11]=>
  string(6) "branch"
}
array(11) {
  [0]=>
  string(5) "unset"
  [1]=>
  string(6) "sunset"
  [2]=>
  string(4) "deep"
  [3]=>
  string(4) "dflt"
  [4]=>
  string(5) "dflt2"
  [5]=>
  int(10)
  [6]=>
  string(1) "b"
  [7]=>
  string(4) "noff"
  [8]=>
  int(1)
  [9]=>
  string(5) "iface"
  [10]=>
  string(5) "empty"
}
array(3) {
  [0]=>
  bool(true)
  [1]=>
  int(1)
  [2]=>
  string(12) "parent_check"
}
array(3) {
  [0]=>
  bool(true)
  [1]=>
  string(5) "child"
  [2]=>
  string(12) "parent_check"
}
array(2) {
  [0]=>
  int(1)
  [1]=>
  string(5) "child"
}
array(2) {
  [0]=>
  string(3) "str"
  [1]=>
  string(2) "ns"
}

Warning: Undefined array key "nokey" in %s on line 24
NULL
