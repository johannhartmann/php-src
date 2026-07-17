--TEST--
W01 semantic corpus: reference and indirect writes preserve aliases
--FILE--
<?php
$name = 'target';
$target = 'initial';
$alias =& $$name;

$alias = 'through-alias';
echo "variable:$target:" . $$name . "\n";

$$name = 'through-indirect';
echo "alias:$alias\n";

$array = ['slot' => 10];
$element =& $array['slot'];
$element += 5;
echo "array:{$array['slot']}:$element\n";
?>
--EXPECT--
variable:through-alias:through-alias
alias:through-indirect
array:15:15
