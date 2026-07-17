--TEST--
W01 semantic corpus: array and string copy-on-write separation
--FILE--
<?php
$arrayOriginal = ['key' => 1];
$arrayCopy = $arrayOriginal;
$arrayCopy['key'] = 2;
echo "array:{$arrayOriginal['key']}:{$arrayCopy['key']}\n";

$stringOriginal = 'abc';
$stringCopy = $stringOriginal;
$stringCopy[0] = 'z';
echo "string:$stringOriginal:$stringCopy\n";
?>
--EXPECT--
array:1:2
string:abc:zbc
