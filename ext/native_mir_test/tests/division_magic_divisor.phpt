--TEST--
Native division and remainder by large integer literals use magic numbers
--DESCRIPTION--
Divisors from 3 to PHP_INT_MAX that are no powers of two divide by a
multiply with their magic number: quotients and remainders of boundary and
random dividends match the VM in untyped, typed and loop code.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function u3($x) { return [$x % 3, $x / 3]; }
function t3(int $x): array { return [$x % 3, $x / 3]; }
function l3(int $n, int $base) { $s = 0; for ($i = 0; $i < $n; $i++) { $s ^= ($base + $i) % 3; } return $s; }
function u5($x) { return [$x % 5, $x / 5]; }
function t5(int $x): array { return [$x % 5, $x / 5]; }
function l5(int $n, int $base) { $s = 0; for ($i = 0; $i < $n; $i++) { $s ^= ($base + $i) % 5; } return $s; }
function u6($x) { return [$x % 6, $x / 6]; }
function t6(int $x): array { return [$x % 6, $x / 6]; }
function l6(int $n, int $base) { $s = 0; for ($i = 0; $i < $n; $i++) { $s ^= ($base + $i) % 6; } return $s; }
function u7($x) { return [$x % 7, $x / 7]; }
function t7(int $x): array { return [$x % 7, $x / 7]; }
function l7(int $n, int $base) { $s = 0; for ($i = 0; $i < $n; $i++) { $s ^= ($base + $i) % 7; } return $s; }
function u9($x) { return [$x % 9, $x / 9]; }
function t9(int $x): array { return [$x % 9, $x / 9]; }
function l9(int $n, int $base) { $s = 0; for ($i = 0; $i < $n; $i++) { $s ^= ($base + $i) % 9; } return $s; }
function u10($x) { return [$x % 10, $x / 10]; }
function t10(int $x): array { return [$x % 10, $x / 10]; }
function l10(int $n, int $base) { $s = 0; for ($i = 0; $i < $n; $i++) { $s ^= ($base + $i) % 10; } return $s; }
function u12($x) { return [$x % 12, $x / 12]; }
function t12(int $x): array { return [$x % 12, $x / 12]; }
function l12(int $n, int $base) { $s = 0; for ($i = 0; $i < $n; $i++) { $s ^= ($base + $i) % 12; } return $s; }
function u60($x) { return [$x % 60, $x / 60]; }
function t60(int $x): array { return [$x % 60, $x / 60]; }
function l60(int $n, int $base) { $s = 0; for ($i = 0; $i < $n; $i++) { $s ^= ($base + $i) % 60; } return $s; }
function u100($x) { return [$x % 100, $x / 100]; }
function t100(int $x): array { return [$x % 100, $x / 100]; }
function l100(int $n, int $base) { $s = 0; for ($i = 0; $i < $n; $i++) { $s ^= ($base + $i) % 100; } return $s; }
function u641($x) { return [$x % 641, $x / 641]; }
function t641(int $x): array { return [$x % 641, $x / 641]; }
function l641(int $n, int $base) { $s = 0; for ($i = 0; $i < $n; $i++) { $s ^= ($base + $i) % 641; } return $s; }
function u1000($x) { return [$x % 1000, $x / 1000]; }
function t1000(int $x): array { return [$x % 1000, $x / 1000]; }
function l1000(int $n, int $base) { $s = 0; for ($i = 0; $i < $n; $i++) { $s ^= ($base + $i) % 1000; } return $s; }
function u86400($x) { return [$x % 86400, $x / 86400]; }
function t86400(int $x): array { return [$x % 86400, $x / 86400]; }
function l86400(int $n, int $base) { $s = 0; for ($i = 0; $i < $n; $i++) { $s ^= ($base + $i) % 86400; } return $s; }
function u1000000007($x) { return [$x % 1000000007, $x / 1000000007]; }
function t1000000007(int $x): array { return [$x % 1000000007, $x / 1000000007]; }
function l1000000007(int $n, int $base) { $s = 0; for ($i = 0; $i < $n; $i++) { $s ^= ($base + $i) % 1000000007; } return $s; }
function u2147483647($x) { return [$x % 2147483647, $x / 2147483647]; }
function t2147483647(int $x): array { return [$x % 2147483647, $x / 2147483647]; }
function l2147483647(int $n, int $base) { $s = 0; for ($i = 0; $i < $n; $i++) { $s ^= ($base + $i) % 2147483647; } return $s; }
function u2147483649($x) { return [$x % 2147483649, $x / 2147483649]; }
function t2147483649(int $x): array { return [$x % 2147483649, $x / 2147483649]; }
function l2147483649(int $n, int $base) { $s = 0; for ($i = 0; $i < $n; $i++) { $s ^= ($base + $i) % 2147483649; } return $s; }
function u4294967297($x) { return [$x % 4294967297, $x / 4294967297]; }
function t4294967297(int $x): array { return [$x % 4294967297, $x / 4294967297]; }
function l4294967297(int $n, int $base) { $s = 0; for ($i = 0; $i < $n; $i++) { $s ^= ($base + $i) % 4294967297; } return $s; }
function u6700417($x) { return [$x % 6700417, $x / 6700417]; }
function t6700417(int $x): array { return [$x % 6700417, $x / 6700417]; }
function l6700417(int $n, int $base) { $s = 0; for ($i = 0; $i < $n; $i++) { $s ^= ($base + $i) % 6700417; } return $s; }
function u1099511627775($x) { return [$x % 1099511627775, $x / 1099511627775]; }
function t1099511627775(int $x): array { return [$x % 1099511627775, $x / 1099511627775]; }
function l1099511627775(int $n, int $base) { $s = 0; for ($i = 0; $i < $n; $i++) { $s ^= ($base + $i) % 1099511627775; } return $s; }
function u4611686018427387903($x) { return [$x % 4611686018427387903, $x / 4611686018427387903]; }
function t4611686018427387903(int $x): array { return [$x % 4611686018427387903, $x / 4611686018427387903]; }
function l4611686018427387903(int $n, int $base) { $s = 0; for ($i = 0; $i < $n; $i++) { $s ^= ($base + $i) % 4611686018427387903; } return $s; }
function u9223372036854775807($x) { return [$x % 9223372036854775807, $x / 9223372036854775807]; }
function t9223372036854775807(int $x): array { return [$x % 9223372036854775807, $x / 9223372036854775807]; }
function l9223372036854775807(int $n, int $base) { $s = 0; for ($i = 0; $i < $n; $i++) { $s ^= ($base + $i) % 9223372036854775807; } return $s; }
$vals = [0, 1, -1, 2, -2, 3, -3, 5, -5, 6, -6, 7, -7, 9, -9, 10, -10, 99, -99, 100, -100, 999, -999, 1000, -1000, PHP_INT_MAX, PHP_INT_MIN, PHP_INT_MAX - 1, PHP_INT_MIN + 1, 1923567077, -13785, -3943579215099822127, -7486979218489765844, 355571806, 210, -1016985127475, -33, -3994940528, 27, 253877687, 984079236132, 17122, 4129386155940464882, 40, -990433715691, -59430, -803766757684, -347295165, 5333, 37, 258803033, -5889558723019949419, 42, -684450654904, -1729022509, -959014181853, 257954022837, -469, -667843089844481693, -634533587878939920, -99113064568, -859994194, 38585, 26083, -7713413546310296580, 52829167702, -536280376, -1955928912124523461, -936388018887, 7, 708506837, -1494715050, -2483786646, 2440235555600920455, 638116287475, -2949221953391443507, 870408478754425468, 2490630940, 295334610, 228, -111246806, -4012167022673272549, 119, 1357024124278566348, 399402305804, -3973511123946863759, 3569816581452282448, -4403579788995504578, -2768260566];
$out = [];
foreach ($vals as $v) { $out[] = json_encode(u3($v)) . json_encode(t3($v)); } $out[] = l3(50, -25) . "," . l3(50, PHP_INT_MAX - 60) . "," . l3(50, PHP_INT_MIN);
foreach ($vals as $v) { $out[] = json_encode(u5($v)) . json_encode(t5($v)); } $out[] = l5(50, -25) . "," . l5(50, PHP_INT_MAX - 60) . "," . l5(50, PHP_INT_MIN);
foreach ($vals as $v) { $out[] = json_encode(u6($v)) . json_encode(t6($v)); } $out[] = l6(50, -25) . "," . l6(50, PHP_INT_MAX - 60) . "," . l6(50, PHP_INT_MIN);
foreach ($vals as $v) { $out[] = json_encode(u7($v)) . json_encode(t7($v)); } $out[] = l7(50, -25) . "," . l7(50, PHP_INT_MAX - 60) . "," . l7(50, PHP_INT_MIN);
foreach ($vals as $v) { $out[] = json_encode(u9($v)) . json_encode(t9($v)); } $out[] = l9(50, -25) . "," . l9(50, PHP_INT_MAX - 60) . "," . l9(50, PHP_INT_MIN);
foreach ($vals as $v) { $out[] = json_encode(u10($v)) . json_encode(t10($v)); } $out[] = l10(50, -25) . "," . l10(50, PHP_INT_MAX - 60) . "," . l10(50, PHP_INT_MIN);
foreach ($vals as $v) { $out[] = json_encode(u12($v)) . json_encode(t12($v)); } $out[] = l12(50, -25) . "," . l12(50, PHP_INT_MAX - 60) . "," . l12(50, PHP_INT_MIN);
foreach ($vals as $v) { $out[] = json_encode(u60($v)) . json_encode(t60($v)); } $out[] = l60(50, -25) . "," . l60(50, PHP_INT_MAX - 60) . "," . l60(50, PHP_INT_MIN);
foreach ($vals as $v) { $out[] = json_encode(u100($v)) . json_encode(t100($v)); } $out[] = l100(50, -25) . "," . l100(50, PHP_INT_MAX - 60) . "," . l100(50, PHP_INT_MIN);
foreach ($vals as $v) { $out[] = json_encode(u641($v)) . json_encode(t641($v)); } $out[] = l641(50, -25) . "," . l641(50, PHP_INT_MAX - 60) . "," . l641(50, PHP_INT_MIN);
foreach ($vals as $v) { $out[] = json_encode(u1000($v)) . json_encode(t1000($v)); } $out[] = l1000(50, -25) . "," . l1000(50, PHP_INT_MAX - 60) . "," . l1000(50, PHP_INT_MIN);
foreach ($vals as $v) { $out[] = json_encode(u86400($v)) . json_encode(t86400($v)); } $out[] = l86400(50, -25) . "," . l86400(50, PHP_INT_MAX - 60) . "," . l86400(50, PHP_INT_MIN);
foreach ($vals as $v) { $out[] = json_encode(u1000000007($v)) . json_encode(t1000000007($v)); } $out[] = l1000000007(50, -25) . "," . l1000000007(50, PHP_INT_MAX - 60) . "," . l1000000007(50, PHP_INT_MIN);
foreach ($vals as $v) { $out[] = json_encode(u2147483647($v)) . json_encode(t2147483647($v)); } $out[] = l2147483647(50, -25) . "," . l2147483647(50, PHP_INT_MAX - 60) . "," . l2147483647(50, PHP_INT_MIN);
foreach ($vals as $v) { $out[] = json_encode(u2147483649($v)) . json_encode(t2147483649($v)); } $out[] = l2147483649(50, -25) . "," . l2147483649(50, PHP_INT_MAX - 60) . "," . l2147483649(50, PHP_INT_MIN);
foreach ($vals as $v) { $out[] = json_encode(u4294967297($v)) . json_encode(t4294967297($v)); } $out[] = l4294967297(50, -25) . "," . l4294967297(50, PHP_INT_MAX - 60) . "," . l4294967297(50, PHP_INT_MIN);
foreach ($vals as $v) { $out[] = json_encode(u6700417($v)) . json_encode(t6700417($v)); } $out[] = l6700417(50, -25) . "," . l6700417(50, PHP_INT_MAX - 60) . "," . l6700417(50, PHP_INT_MIN);
foreach ($vals as $v) { $out[] = json_encode(u1099511627775($v)) . json_encode(t1099511627775($v)); } $out[] = l1099511627775(50, -25) . "," . l1099511627775(50, PHP_INT_MAX - 60) . "," . l1099511627775(50, PHP_INT_MIN);
foreach ($vals as $v) { $out[] = json_encode(u4611686018427387903($v)) . json_encode(t4611686018427387903($v)); } $out[] = l4611686018427387903(50, -25) . "," . l4611686018427387903(50, PHP_INT_MAX - 60) . "," . l4611686018427387903(50, PHP_INT_MIN);
foreach ($vals as $v) { $out[] = json_encode(u9223372036854775807($v)) . json_encode(t9223372036854775807($v)); } $out[] = l9223372036854775807(50, -25) . "," . l9223372036854775807(50, PHP_INT_MAX - 60) . "," . l9223372036854775807(50, PHP_INT_MIN);
echo md5(implode("\n", $out)), " ", count($out), "\n";
foreach ([0, 7, 41, 100, 1799] as $i) echo $out[$i], "\n";
?>
--EXPECT--
e1dd24e58c9045a70e8c9c7d7e491369 1800
[0,0][0,0]
[2,1.6666666666666667][2,1.6666666666666667]
[1,5707.333333333333][1,5707.333333333333]
[-1,-1.2][-1,-1.2]
-1,55,-1
