--TEST--
Native includes of cached scripts end their op array when they return
--DESCRIPTION--
As in the VM, every include of a cached script runs a fresh copy of its main
op array: file-scope static variables start over, the including scope's
variables are visible, a nested include and a throwing include unwind, and
closures created by the include keep their own statics.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
$dir = __DIR__;
$p = "$dir/opcache_cached_include_lifetime";
file_put_contents("$p.s.inc", '<?php static $n = 0; $n++; return $n;');
file_put_contents("$p.v.inc", '<?php $seen = isset($local) ? $local : "unset"; $local = "set by include"; return [$seen, $x ?? null];');
file_put_contents("$p.n.inc", '<?php return (include __DIR__ . "/opcache_cached_include_lifetime.s.inc") + 10;');
file_put_contents("$p.e.inc", '<?php if ($throw) { throw new RuntimeException("from include"); } return "no throw";');
file_put_contents("$p.c.inc", '<?php return function ($y) use ($k) { static $calls = 0; return [$k, $y, ++$calls]; };');
$load = \Closure::bind(static function ($file) { return include $file; }, null, null);
for ($i = 0; $i < 3; $i++) {
    var_dump(include "$p.s.inc");
    var_dump($load("$p.s.inc"));
    var_dump(include "$p.n.inc");
}
function scoped($p) { $x = 5; $r = include "$p.v.inc"; return [$r, $local]; }
var_dump(scoped($p), scoped($p));
$x = 7; var_dump(include "$p.v.inc", $local);
foreach ([false, true, false] as $throw) {
    try { var_dump(include "$p.e.inc"); } catch (RuntimeException $e) { echo get_class($e), ': ', $e->getMessage(), ' line ', $e->getLine(), "\n"; }
}
$fs = [];
foreach (['a', 'b'] as $k) { $fs[] = include "$p.c.inc"; }
foreach ($fs as $f) { var_dump($f(1), $f(2)); }
?>
--CLEAN--
<?php
foreach (['s', 'v', 'n', 'e', 'c'] as $suffix) {
    @unlink(__DIR__ . "/opcache_cached_include_lifetime.$suffix.inc");
}
?>
--EXPECT--
int(1)
int(1)
int(11)
int(1)
int(1)
int(11)
int(1)
int(1)
int(11)
array(2) {
  [0]=>
  array(2) {
    [0]=>
    string(5) "unset"
    [1]=>
    int(5)
  }
  [1]=>
  string(14) "set by include"
}
array(2) {
  [0]=>
  array(2) {
    [0]=>
    string(5) "unset"
    [1]=>
    int(5)
  }
  [1]=>
  string(14) "set by include"
}
array(2) {
  [0]=>
  string(5) "unset"
  [1]=>
  int(7)
}
string(14) "set by include"
string(8) "no throw"
RuntimeException: from include line 1
string(8) "no throw"
array(3) {
  [0]=>
  string(1) "a"
  [1]=>
  int(1)
  [2]=>
  int(1)
}
array(3) {
  [0]=>
  string(1) "a"
  [1]=>
  int(2)
  [2]=>
  int(2)
}
array(3) {
  [0]=>
  string(1) "b"
  [1]=>
  int(1)
  [2]=>
  int(1)
}
array(3) {
  [0]=>
  string(1) "b"
  [1]=>
  int(2)
  [2]=>
  int(2)
}
