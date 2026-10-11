--TEST--
Native undefined variable warnings keep a NUL byte in the variable name
--DESCRIPTION--
Every native helper reports an undefined CV as zval_undefined_cv() does ("$%S"), so a CV name containing a NUL byte is printed whole.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
set_error_handler(function ($no, $msg) { echo "E: ", str_replace("\0", '\0', $msg), "\n"; return true; });
class P { public $q = 1; }
function gen() { yield ${"y\0k"} => ${"y\0v"}; }
function nul($i) {
    echo ${"e\0cho"};
    $a = ${"a\0b"} . 'x';
    $b = 'x' . ${"c\0d"} . "y{${"r\0p"}}z";
    ${"o\0p"} .= 'x';
    ${"i\0d"}++;
    $t = is_int(${"t\0c"});
    $q = ${"q\0m"} ?? 'd';
    $c = ${"q\0n"};
    ${"d\0m"}['x'] .= 'y';
    $n = strlen(${"s\0a"});
    $o = new P;
    $o->q += ${"o\0d"};
    $o->{${"p\0n"}} = 1;
    $cls = get_class(${"g\0c"} ?? $o);
    try { $cn = new ${"n\0c"}; } catch (Error $e) { echo $e->getMessage(), "\n"; }
    foreach (gen() as $k => $v) { var_dump($k, $v); }
    return [$a, $b, ${"o\0p"}, ${"i\0d"}, $t, $q, $c, ${"d\0m"}, $n, $o->q, $cls];
}
var_dump(nul(1));
function ret() { return ${"r\0et"}; }
var_dump(ret());
--EXPECT--
E: Undefined variable $e\0cho
E: Undefined variable $a\0b
E: Undefined variable $c\0d
E: Undefined variable $r\0p
E: Undefined variable $o\0p
E: Undefined variable $i\0d
E: Undefined variable $t\0c
E: Undefined variable $q\0n
E: Undefined variable $d\0m
E: Undefined array key "x"
E: Undefined variable $s\0a
E: strlen(): Passing null to parameter #1 ($string) of type string is deprecated
E: Undefined variable $o\0d
E: Undefined variable $p\0n
E: Creation of dynamic property P::$ is deprecated
E: Undefined variable $n\0c
Class name must be a valid object or a string
E: Undefined variable $y\0v
E: Undefined variable $y\0k
NULL
NULL
array(11) {
  [0]=>
  string(1) "x"
  [1]=>
  string(3) "xyz"
  [2]=>
  string(1) "x"
  [3]=>
  int(1)
  [4]=>
  bool(false)
  [5]=>
  string(1) "d"
  [6]=>
  NULL
  [7]=>
  array(1) {
    ["x"]=>
    string(1) "y"
  }
  [8]=>
  int(0)
  [9]=>
  int(1)
  [10]=>
  string(1) "P"
}
E: Undefined variable $r\0et
NULL
