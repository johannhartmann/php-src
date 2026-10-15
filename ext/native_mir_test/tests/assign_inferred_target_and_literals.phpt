--TEST--
Native assignments release, dereference and copy as far as the inferred types require
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class D { public $n; function __construct($n) { $this->n = $n; } function __destruct() { echo "~", $this->n, " "; } }
function literals() { return [1, 2, 3]; }
function run(int $n) {
    $x = 0;
    $x = 1.5;
    $x = null;
    $x = true;
    $x = "lit";
    $x = [1, 2];
    $list = literals();
    $copy = $list;
    $copy[] = $n;
    $object = new D(1);
    $object = new D(2);
    echo "| ";
    $object = 3;
    static $kept = 0;
    $kept = $n;
    $kept = [$n];
    global $shared;
    $shared = $n * 2;
    $alias = &$x;
    $alias = "via ref";
    $chained = $x = $n + 1;
    $y = $maybe ?? "default";
    return [$x, count($list), count($copy), $kept, $shared, $chained, $y];
}
var_dump(run(5));
echo "\n";
?>
--EXPECT--
~1 | ~2 array(7) {
  [0]=>
  int(6)
  [1]=>
  int(3)
  [2]=>
  int(4)
  [3]=>
  array(1) {
    [0]=>
    int(5)
  }
  [4]=>
  int(10)
  [5]=>
  int(6)
  [6]=>
  string(7) "default"
}
