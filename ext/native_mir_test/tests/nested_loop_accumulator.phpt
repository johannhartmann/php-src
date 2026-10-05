--TEST--
Accumulators carried through nested loops reach the return
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function sum_rows(array $m, int $n, $start) {
    $s = $start;
    for ($i = 0; $i < $n; $i++) {
        $row = $m[$i];
        for ($j = 0; $j < $n; $j++) {
            $s += $row[$j];
        }
    }
    return $s;
}
function sum_cube(array $c, int $n, $start) {
    $s = $start;
    for ($i = 0; $i < $n; $i++) {
        for ($j = 0; $j < $n; $j++) {
            for ($k = 0; $k < $n; $k++) {
                $s += $c[$i][$j][$k];
            }
        }
    }
    return $s;
}
function count_rows(array $m, int $n) {
    $s = 0;
    for ($i = 0; $i < $n; $i++) {
        $row = $m[$i];
        for ($j = 0; $j < $n; $j++) {
            $s += $row[$j];
        }
    }
    return $s;
}
$m = [[0, 0, 0], [0, 1, 2], [0, 2, 4]];
$c = [$m, $m, $m];
for ($r = 0; $r < 3; $r++) {
    var_dump(sum_rows($m, 3, 0), sum_rows($m, 3, 0.5), sum_rows($m, 3, "1"));
    var_dump(sum_rows([[PHP_INT_MAX, 1], [1, 1]], 2, 0));
    var_dump(sum_cube($c, 3, 0), sum_cube($c, 3, 1.5));
    var_dump(count_rows($m, 3), count_rows([[1.5, 2], [3, 4]], 2));
}
?>
--EXPECT--
int(9)
float(9.5)
int(10)
float(9.223372036854776E+18)
int(27)
float(28.5)
int(9)
float(10.5)
int(9)
float(9.5)
int(10)
float(9.223372036854776E+18)
int(27)
float(28.5)
int(9)
float(10.5)
int(9)
float(9.5)
int(10)
float(9.223372036854776E+18)
int(27)
float(28.5)
int(9)
float(10.5)
