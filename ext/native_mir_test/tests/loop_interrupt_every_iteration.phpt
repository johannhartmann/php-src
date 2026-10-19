--TEST--
Native: an interrupt at every loop iteration keeps the loop's values
--DESCRIPTION--
VmInterruptComparable raises EG(vm_interrupt) at each comparison, so the
loop's interrupt poll takes its slow path every iteration. The slow path
saves and restores the registers it may clobber, and the loop's long,
double and string values survive the handler calls.
--EXTENSIONS--
opcache
zend_test
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function f(int $n, array $w) {
    $x = new VmInterruptComparable(1);
    $y = new VmInterruptComparable(2);
    $s = 0.0; $k = 7; $c = 0; $t = "";
    for ($i = 0; $i < $n; $i++) {
        if ($x < $y) { $c++; }
        $s += $w[$i & 3] * 0.5;
        $k = ($k * 31 + $i) & 0xFFFFF;
        $t = $i % 2 ? "odd" : "even";
    }
    return [$s, $k, $c, $t];
}
var_dump(f(1000, [1.5, 2.5, 3.5, 4.5]));
$s = 0.0; $k = 7;
$x = new VmInterruptComparable(1); $y = new VmInterruptComparable(2);
for ($i = 0; $i < 1000; $i++) { $b = $x < $y; $s += $i * 0.25; $k = ($k * 31 + $i) & 0xFFFFF; }
var_dump($s, $k, $b);
?>
--EXPECT--
array(4) {
  [0]=>
  float(1500)
  [1]=>
  int(696059)
  [2]=>
  int(1000)
  [3]=>
  string(3) "odd"
}
float(124875)
int(696059)
bool(true)
