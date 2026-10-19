--TEST--
Native untyped bitwise operations of two longs compute inline
--DESCRIPTION--
&, | and ^ of operands whose types are unknown compute inline when both
are longs; doubles (with their precision deprecation), strings, null,
booleans, arrays and references keep the helper.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function f($a, $b) { return [$a & $b, $a | $b, $a ^ $b]; }
$vals = [0, 1, -1, 5, 12, PHP_INT_MAX, PHP_INT_MIN, 2.0, 2.5, -3.9, 1e20, NAN, "12", "abc", "3.5", "", null, true, false];
foreach ($vals as $a) { foreach ([0, 1, -1, 6, 255, 2.0, "7", "x", null, true] as $b) { try { echo json_encode(@f($a, $b)); } catch (Throwable $e) { echo get_class($e), ":", $e->getMessage(); } echo "\n"; } }
var_dump(f("abc", "ABC"), f("a", 1.5));
try { f([], 1); } catch (TypeError $e) { echo $e->getMessage(), "\n"; }
$x = 6; $r = &$x; var_dump($x & 3, $r | 8);
$s = 0; for ($i = 0; $i < 100; $i++) { $s = ($s + ($i * 7 + 3)) ^ ($s & 1); $s &= 0xFFFFF; } var_dump($s);
var_dump(f(1.5, 1));
?>
--EXPECTF--
[0,0,0]
[0,1,1]
[0,-1,-1]
[0,6,6]
[0,255,255]
[0,2,2]
[0,7,7]
TypeError:Unsupported operand types: int & string
[0,0,0]
[0,1,1]
[0,1,1]
[1,1,0]
[1,-1,-2]
[0,7,7]
[1,255,254]
[0,3,3]
[1,7,6]
TypeError:Unsupported operand types: int & string
[0,1,1]
[1,1,0]
[0,-1,-1]
[1,-1,-2]
[-1,-1,0]
[6,-1,-7]
[255,-1,-256]
[2,-1,-3]
[7,-1,-8]
TypeError:Unsupported operand types: int & string
[0,-1,-1]
[1,-1,-2]
[0,5,5]
[1,5,4]
[5,-1,-6]
[4,7,3]
[5,255,250]
[0,7,7]
[5,7,2]
TypeError:Unsupported operand types: int & string
[0,5,5]
[1,5,4]
[0,12,12]
[0,13,13]
[12,-1,-13]
[4,14,10]
[12,255,243]
[0,14,14]
[4,15,11]
TypeError:Unsupported operand types: int & string
[0,12,12]
[0,13,13]
[0,9223372036854775807,9223372036854775807]
[1,9223372036854775807,9223372036854775806]
[9223372036854775807,-1,-9223372036854775808]
[6,9223372036854775807,9223372036854775801]
[255,9223372036854775807,9223372036854775552]
[2,9223372036854775807,9223372036854775805]
[7,9223372036854775807,9223372036854775800]
TypeError:Unsupported operand types: int & string
[0,9223372036854775807,9223372036854775807]
[1,9223372036854775807,9223372036854775806]
[0,-9223372036854775808,-9223372036854775808]
[0,-9223372036854775807,-9223372036854775807]
[-9223372036854775808,-1,9223372036854775807]
[0,-9223372036854775802,-9223372036854775802]
[0,-9223372036854775553,-9223372036854775553]
[0,-9223372036854775806,-9223372036854775806]
[0,-9223372036854775801,-9223372036854775801]
TypeError:Unsupported operand types: int & string
[0,-9223372036854775808,-9223372036854775808]
[0,-9223372036854775807,-9223372036854775807]
[0,2,2]
[0,3,3]
[2,-1,-3]
[2,6,4]
[2,255,253]
[2,2,0]
[2,7,5]
TypeError:Unsupported operand types: float & string
[0,2,2]
[0,3,3]
[0,2,2]
[0,3,3]
[2,-1,-3]
[2,6,4]
[2,255,253]
[2,2,0]
[2,7,5]
TypeError:Unsupported operand types: float & string
[0,2,2]
[0,3,3]
[0,-3,-3]
[1,-3,-4]
[-3,-1,2]
[4,-1,-5]
[253,-1,-254]
[0,-1,-1]
[5,-1,-6]
TypeError:Unsupported operand types: float & string
[0,-3,-3]
[1,-3,-4]
[0,7766279631452241920,7766279631452241920]
[0,7766279631452241921,7766279631452241921]
[7766279631452241920,-1,-7766279631452241921]
[0,7766279631452241926,7766279631452241926]
[0,7766279631452242175,7766279631452242175]
[0,7766279631452241922,7766279631452241922]
[0,7766279631452241927,7766279631452241927]
TypeError:Unsupported operand types: float & string
[0,7766279631452241920,7766279631452241920]
[0,7766279631452241921,7766279631452241921]
[0,0,0]
[0,1,1]
[0,-1,-1]
[0,6,6]
[0,255,255]
[0,2,2]
[0,7,7]
TypeError:Unsupported operand types: float & string
[0,0,0]
[0,1,1]
[0,12,12]
[0,13,13]
[12,-1,-13]
[4,14,10]
[12,255,243]
[0,14,14]
["1","72","\u0006"]
["0","y2","I"]
[0,12,12]
[0,13,13]
TypeError:Unsupported operand types: string & int
TypeError:Unsupported operand types: string & int
TypeError:Unsupported operand types: string & int
TypeError:Unsupported operand types: string & int
TypeError:Unsupported operand types: string & int
TypeError:Unsupported operand types: string & float
["!","wbc","V"]
["`","ybc","\u0019"]
TypeError:Unsupported operand types: string & null
TypeError:Unsupported operand types: string & bool
[0,3,3]
[1,3,2]
[3,-1,-4]
[2,7,5]
[3,255,252]
[2,3,1]
["3","7.5","\u0004"]
["0","{.5","K"]
[0,3,3]
[1,3,2]
TypeError:Unsupported operand types: string & int
TypeError:Unsupported operand types: string & int
TypeError:Unsupported operand types: string & int
TypeError:Unsupported operand types: string & int
TypeError:Unsupported operand types: string & int
TypeError:Unsupported operand types: string & float
["","7",""]
["","x",""]
TypeError:Unsupported operand types: string & null
TypeError:Unsupported operand types: string & bool
[0,0,0]
[0,1,1]
[0,-1,-1]
[0,6,6]
[0,255,255]
[0,2,2]
[0,7,7]
TypeError:Unsupported operand types: null & string
[0,0,0]
[0,1,1]
[0,1,1]
[1,1,0]
[1,-1,-2]
[0,7,7]
[1,255,254]
[0,3,3]
[1,7,6]
TypeError:Unsupported operand types: bool & string
[0,1,1]
[1,1,0]
[0,0,0]
[0,1,1]
[0,-1,-1]
[0,6,6]
[0,255,255]
[0,2,2]
[0,7,7]
TypeError:Unsupported operand types: bool & string
[0,0,0]
[0,1,1]

Fatal error: Uncaught TypeError: Unsupported operand types: string & float in %s:2
Stack trace:
#0 %s(5): f('a', 1.5)
#1 {main}
  thrown in %s on line 2
