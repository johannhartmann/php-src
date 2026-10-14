--TEST--
Native branch truthiness on operands of inferred types
--DESCRIPTION--
A JMPZ/JMPNZ tests only the types Zend's type inference allows for its
operand; the result must match the VM for every allowed type.
--EXTENSIONS--
native_mir_test
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function s(string $s) { if ($s) { return 1; } return 0; }
function a(array $a) { if ($a) { return 1; } return 0; }
function i(?int $i) { if ($i) { return 1; } return 0; }
function ns(?string $s) { if (!$s) { return 0; } return 1; }
function bi(bool|int $v) { if ($v) { return 1; } return 0; }
function fl(float $f) { if ($f) { return 1; } return 0; }
function o(?object $o) { if ($o) { return 1; } return 0; }
function u($x) { if ($x > 1) { $y = 1; } if ($y) { return 1; } return 0; }
function r($r) { if ($r) { return 1; } return 0; }
function ref(&$v) { if ($v) { return 1; } return 0; }
function local($n) { $t = $n > 2 ? 'x' : ''; if ($t) { return 1; } return 0; }
function mixed_arr($n) { $t = $n > 2 ? [1] : null; if ($t) { return 1; } return 0; }
$f = fopen('php://memory', 'r');
$out = '';
for ($k = 0; $k < 40; $k++) {
    $out = '';
    foreach (['', '0', '00', 'a', str_repeat('0', 1)] as $v) { $out .= s($v); }
    foreach ([[], [0], range(1, 2)] as $v) { $out .= a($v); }
    foreach ([null, 0, 7, -1] as $v) { $out .= i($v); }
    foreach ([null, '', '0', 'x'] as $v) { $out .= ns($v); }
    foreach ([true, false, 0, 3] as $v) { $out .= bi($v); }
    foreach ([0.0, -0.0, 0.5, INF] as $v) { $out .= fl($v); }
    foreach ([null, new stdClass] as $v) { $out .= o($v); }
    foreach ([0, 3] as $v) { $out .= @u($v); }
    $out .= r($f);
    foreach ([0, 'a', '', [1], null] as $v) { $out .= ref($v); }
    foreach ([1, 3] as $v) { $out .= local($v) . mixed_arr($v); }
}
echo $out, "\n";
?>
--EXPECT--
00110011001100011001001101011010100011
