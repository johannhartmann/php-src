--TEST--
W01 semantic corpus: array and string copy-on-write separation
--FILE--
<?php
$arrayOriginal = ['key' => 1];
$arrayCopy = $arrayOriginal;
echo "array:before:{$arrayOriginal['key']}:{$arrayCopy['key']}\n";
$arrayCopy['key'] = 2;
echo "array:after:{$arrayOriginal['key']}:{$arrayCopy['key']}\n";

$stringOriginal = 'abc';
$stringCopy = $stringOriginal;
echo "string:before:$stringOriginal:$stringCopy\n";
$stringCopy[0] = 'z';
echo "string:after:$stringOriginal:$stringCopy\n";
?>
--EXPECT--
array:before:1:1
array:after:1:2
string:before:abc:abc
string:after:abc:zbc
