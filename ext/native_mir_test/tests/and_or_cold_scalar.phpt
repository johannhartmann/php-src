--TEST--
Native &&/|| and if of a CV decide null, booleans and integers in the cold block
--DESCRIPTION--
JMPZ_EX/JMPNZ_EX whose branch PHI inputs the cold block defines publish the
boolean result of a null, boolean or integer condition without the helper;
JMPZ/JMPNZ of such a CV decide likewise. Strings, arrays, objects,
references and undefined CVs keep the helper.
--EXTENSIONS--
native_mir_test
--FILE--
<?php
function chain($a, $b, $c) {
    $x = $a && $b;
    $y = $a || $c;
    $z = ($a && $b) || $c;
    return var_export([$x, $y, $z], true);
}
function cv_if($v) {
    if ($v) { return 'T'; }
    return 'F';
}
$vals = [null, false, true, 0, 1, -5, '', '0', 'a', [], [1], new stdClass];
$out = [];
foreach ($vals as $a) {
    foreach ([true, 0, 'x'] as $b) {
        $out[] = str_replace(["\n", ' '], '', chain($a, $b, $a));
    }
    $out[] = cv_if($a);
}
$r = 1;
$ref = &$r;
$out[] = cv_if($ref);
echo implode('|', $out), "\n";
function undef_cv() { if ($nope) { return 1; } return 0; }
echo undef_cv(), "\n";
?>
--EXPECTF--
array(0=>false,1=>false,2=>false,)|array(0=>false,1=>false,2=>false,)|array(0=>false,1=>false,2=>false,)|F|array(0=>false,1=>false,2=>false,)|array(0=>false,1=>false,2=>false,)|array(0=>false,1=>false,2=>false,)|F|array(0=>true,1=>true,2=>true,)|array(0=>false,1=>true,2=>true,)|array(0=>true,1=>true,2=>true,)|T|array(0=>false,1=>false,2=>false,)|array(0=>false,1=>false,2=>false,)|array(0=>false,1=>false,2=>false,)|F|array(0=>true,1=>true,2=>true,)|array(0=>false,1=>true,2=>true,)|array(0=>true,1=>true,2=>true,)|T|array(0=>true,1=>true,2=>true,)|array(0=>false,1=>true,2=>true,)|array(0=>true,1=>true,2=>true,)|T|array(0=>false,1=>false,2=>false,)|array(0=>false,1=>false,2=>false,)|array(0=>false,1=>false,2=>false,)|F|array(0=>false,1=>false,2=>false,)|array(0=>false,1=>false,2=>false,)|array(0=>false,1=>false,2=>false,)|F|array(0=>true,1=>true,2=>true,)|array(0=>false,1=>true,2=>true,)|array(0=>true,1=>true,2=>true,)|T|array(0=>false,1=>false,2=>false,)|array(0=>false,1=>false,2=>false,)|array(0=>false,1=>false,2=>false,)|F|array(0=>true,1=>true,2=>true,)|array(0=>false,1=>true,2=>true,)|array(0=>true,1=>true,2=>true,)|T|array(0=>true,1=>true,2=>true,)|array(0=>false,1=>true,2=>true,)|array(0=>true,1=>true,2=>true,)|T|T

Warning: Undefined variable $nope in %s on line %d
0
