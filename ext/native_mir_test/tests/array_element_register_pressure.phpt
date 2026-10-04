--TEST--
Native array element reads under register pressure
--DESCRIPTION--
Element reads and isset() with integer, string and boxed keys while many
locals stay live. Where too few registers remain for a lookup snippet, the
fast path goes to the guarded cold block instead of failing compilation.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function native_pressure(array $list, array $map, int $n, string $name) {
    $v0 = $n + 0; $s0 = $name . 0;
    $v1 = $n + 1; $s1 = $name . 1;
    $v2 = $n + 2; $s2 = $name . 2;
    $v3 = $n + 3; $s3 = $name . 3;
    $v4 = $n + 4; $s4 = $name . 4;
    $v5 = $n + 5; $s5 = $name . 5;
    $v6 = $n + 6; $s6 = $name . 6;
    $v7 = $n + 7; $s7 = $name . 7;
    $v8 = $n + 8; $s8 = $name . 8;
    $v9 = $n + 9; $s9 = $name . 9;
    $v10 = $n + 10; $s10 = $name . 10;
    $v11 = $n + 11; $s11 = $name . 11;
    $v12 = $n + 12; $s12 = $name . 12;
    $v13 = $n + 13; $s13 = $name . 13;
    $v14 = $n + 14; $s14 = $name . 14;
    $v15 = $n + 15; $s15 = $name . 15;
    $v16 = $n + 16; $s16 = $name . 16;
    $v17 = $n + 17; $s17 = $name . 17;
    $v18 = $n + 18; $s18 = $name . 18;
    $v19 = $n + 19; $s19 = $name . 19;
    $v20 = $n + 20; $s20 = $name . 20;
    $v21 = $n + 21; $s21 = $name . 21;
    $v22 = $n + 22; $s22 = $name . 22;
    $v23 = $n + 23; $s23 = $name . 23;
    $out = [];
    for ($i = 0; $i < 3; $i++) {
        $k = $i + 1; $key = $i ? $name : 'b';
        $out[] = $list[$k] + $list[$i * 2] + ($map[$key] ?? 0) + $map[$s3 === 'x3' ? 'x3' : 'b'];
        $out[] = isset($map[$key]) ? $map[$key] : -1;
        $out[] = $v0 + $v1 + $v2 + $v3 + $v4 + $v5 + $v6 + $v7 + $v8 + $v9 + $v10 + $v11 + $v12 + $v13 + $v14 + $v15 + $v16 + $v17 + $v18 + $v19 + $v20 + $v21 + $v22 + $v23;
        $out[] = $s0 . $s4 . $s8 . $s12 . $s16 . $s20;
    }
    return $out;
}
$list = range(10, 20); $map = ['b' => 2, 'x' => 5, 'x3' => 7];
for ($r = 0; $r < 3; $r++) { echo json_encode(native_pressure($list, $map, $r, 'x')), "\n"; }

?>
--EXPECT--
[30,2,276,"x0x4x8x12x16x20",36,5,276,"x0x4x8x12x16x20",39,5,276,"x0x4x8x12x16x20"]
[30,2,300,"x0x4x8x12x16x20",36,5,300,"x0x4x8x12x16x20",39,5,300,"x0x4x8x12x16x20"]
[30,2,324,"x0x4x8x12x16x20",36,5,324,"x0x4x8x12x16x20",39,5,324,"x0x4x8x12x16x20"]
