--TEST--
Native property reads dereference references and FETCH_OBJ_FUNC_ARG reads inline
--DESCRIPTION--
A cached declared property holding a reference reads as its value, and a
property passed as an argument reads like FETCH_OBJ_R when the parameter
takes it by value; a by-reference parameter keeps the write fetch.
--EXTENSIONS--
native_mir_test
--FILE--
<?php
class P {
  public $a = 1; public $arr = [1, 2]; public $ref; public $s = "str"; public $o;
  function __construct() { $x = 5; $this->ref = &$x; $this->o = new stdClass; }
  function readref() { return $this->ref; }
  function readref2() { $v = $this->ref; $v++; return [$v, $this->ref]; }
  function fa() { return strlen($this->s) + count($this->arr) + byval($this->a) + byval($this->ref); }
  function fr() { byref($this->arr); byref($this->ref); return [$this->arr, $this->ref]; }
  function fmix() { return mix($this->a, $this->arr); }
}
function byval($v) { return $v * 2; }
function byref(&$v) { if (is_array($v)) $v[] = 9; else $v++; }
function mix($a, &$b) { $b[] = $a; return count($b); }
function dyn($o, $n) { return byval($o->$n); }
for ($i = 0; $i < 3; $i++) {
  $p = new P;
  var_dump($p->readref(), $p->readref2(), $p->fa(), $p->fr(), $p->fmix(), $p->arr, dyn($p, 'a'));
  $p->ref = [1]; var_dump($p->readref()); $p->ref = 7;
  $r = &$p->a; $r = 42; var_dump($p->fa(), $p->a);
  $q = new P; unset($q->a); try { var_dump(byval($q->a)); } catch (Error $e) {}
}

?>
--EXPECTF--
int(5)
array(2) {
  [0]=>
  int(6)
  [1]=>
  int(5)
}
int(17)
array(2) {
  [0]=>
  array(3) {
    [0]=>
    int(1)
    [1]=>
    int(2)
    [2]=>
    int(9)
  }
  [1]=>
  int(6)
}
int(4)
array(4) {
  [0]=>
  int(1)
  [1]=>
  int(2)
  [2]=>
  int(9)
  [3]=>
  int(1)
}
int(2)
array(1) {
  [0]=>
  int(1)
}
int(105)
int(42)

Warning: Undefined property: P::$a in %s on line 20
int(0)
int(5)
array(2) {
  [0]=>
  int(6)
  [1]=>
  int(5)
}
int(17)
array(2) {
  [0]=>
  array(3) {
    [0]=>
    int(1)
    [1]=>
    int(2)
    [2]=>
    int(9)
  }
  [1]=>
  int(6)
}
int(4)
array(4) {
  [0]=>
  int(1)
  [1]=>
  int(2)
  [2]=>
  int(9)
  [3]=>
  int(1)
}
int(2)
array(1) {
  [0]=>
  int(1)
}
int(105)
int(42)

Warning: Undefined property: P::$a in %s on line 20
int(0)
int(5)
array(2) {
  [0]=>
  int(6)
  [1]=>
  int(5)
}
int(17)
array(2) {
  [0]=>
  array(3) {
    [0]=>
    int(1)
    [1]=>
    int(2)
    [2]=>
    int(9)
  }
  [1]=>
  int(6)
}
int(4)
array(4) {
  [0]=>
  int(1)
  [1]=>
  int(2)
  [2]=>
  int(9)
  [3]=>
  int(1)
}
int(2)
array(1) {
  [0]=>
  int(1)
}
int(105)
int(42)

Warning: Undefined property: P::$a in %s on line 20
int(0)
