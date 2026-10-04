--TEST--
Native compound array updates in place and foreach into plain CVs
--DESCRIPTION--
$a[$k] += $n updates an existing numeric element of an unshared array in
place; overflow, references, strings, shared arrays and missing keys take the
general path. foreach copies into a CV without the assignment protocol when
the CV holds no counted value; destructors still run in order.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function f($n) { $h1 = []; $h2 = []; for ($i = 0; $i < $n; $i++) { $h1["k$i"] = $i; $h2["k$i"] = 0; }
  $h2[5] = 1.5; $h1[5] = 2; $h2["big"] = PHP_INT_MAX; $h1["big"] = 1; $h2["s"] = "3"; $h1["s"] = 4;
  for ($r = 0; $r < 2; $r++) foreach ($h1 as $k => $v) { $h2[$k] += $v; }
  $c = $h2; $c["k1"] -= 10; $c["k2"] *= 3; $c["k3"] *= 2.5; $c["k4"] += 0.5;
  $x = [1]; $y = &$x[0]; $x[0] += 5;
  try { $h2["missing"] += 1; } catch (Throwable $e) {}
  return json_encode([$h2, $c["k1"], $c["k2"], $c["k3"], $c["k4"], $h2["k1"], $x, $y]); }
echo f(5), "\n", f(5), "\n";
class D { function __destruct() { echo "d"; } }
function fe() { $o = ""; $a = [1, "x", 2.5, [1], null, new D, "y"]; foreach ($a as $k => $v) { $o .= $k . gettype($v); } $v = null; return $o; }
function g() { $a = [1, 2]; $v = 7; $r = &$v; foreach ($a as $v) {} return "$v $r"; }
function h() { $a = ["a" => new D]; foreach ($a as $v) {} unset($a); echo "|"; $v = 1; return "h"; }
echo fe(), " ", g(), " ", h(), "\n"; echo fe(), "\n";
?>
--EXPECTF--

Warning: Undefined array key "missing" in %s on line %d
[{"k0":0,"k1":2,"k2":4,"k3":6,"k4":8,"5":5.5,"big":9.223372036854776e+18,"s":11,"missing":1},-8,12,15,8.5,2,[6],6]

Warning: Undefined array key "missing" in %s on line %d
[{"k0":0,"k1":2,"k2":4,"k3":6,"k4":8,"5":5.5,"big":9.223372036854776e+18,"s":11,"missing":1},-8,12,15,8.5,2,[6],6]
d0integer1string2double3array4NULL5object6string 2 2 |dh
d0integer1string2double3array4NULL5object6string
