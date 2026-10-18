--TEST--
Native: a switch whose cases share a target with phi inputs
--DESCRIPTION--
The optimizer sends several cases and the default of SWITCH_STRING,
SWITCH_LONG and MATCH to one block. Each distinct target takes one edge
with its phi moves, as TPDE counts one phi input per predecessor block.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function str_tmp($x) {
    $a = false;
    switch ($x["y"]) {
        case "a": $a = true; break;
        case "b": break;
        case "c": break;
    }
    return $a;
}
function str_two($x) {
    $a = 0;
    $b = 10;
    switch ($x["y"]) {
        case "a": $a = 1; $b = 11; break;
        case "b": $a = 2; break;
        case "c": case "d": break;
    }
    return [$a, $b];
}
function long_tmp($x) {
    $a = false;
    switch ($x["y"]) {
        case 1: $a = true; break;
        case 2: break;
        case 3: break;
    }
    return $a;
}
function long_loop($list) {
    $n = 0;
    foreach ($list as $v) {
        switch ($v + 0) {
            case 1: $n += 10; break;
            case 2: case 3: break;
            case 4: $n += 1; break;
        }
    }
    return $n;
}
function match_dup($x) {
    $r = 'none';
    $k = match ($x["y"]) {
        "a", "b" => 1,
        "c" => 2,
        default => 1,
    };
    if ($k === 2) { $r = 'two'; }
    return [$k, $r];
}
foreach ([["y" => "a"], ["y" => "b"], ["y" => "c"], ["y" => "d"], ["y" => 1], ["y" => 2], ["y" => 3], ["y" => 4], ["y" => 1.0], ["y" => null]] as $x) {
    echo json_encode([str_tmp($x), str_two($x), long_tmp($x), match_dup($x)]), "\n";
}
var_dump(long_loop([1, 2, 3, 4, 1, 5, 4]));
?>
--EXPECT--
[true,[1,11],false,[1,"none"]]
[false,[2,10],false,[1,"none"]]
[false,[0,10],false,[2,"two"]]
[false,[0,10],false,[1,"none"]]
[false,[0,10],true,[1,"none"]]
[false,[0,10],false,[1,"none"]]
[false,[0,10],false,[1,"none"]]
[false,[0,10],false,[1,"none"]]
[false,[0,10],true,[1,"none"]]
[false,[0,10],false,[1,"none"]]
int(22)
