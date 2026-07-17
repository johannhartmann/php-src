--TEST--
W01 semantic corpus: eval and include create dynamic definitions
--FILE--
<?php
eval('function evaluatedValue(): string { return "eval"; }');
include __DIR__ . '/dynamic_definitions.inc';

echo evaluatedValue(), "\n";
echo includedValue(), "\n";
?>
--EXPECT--
eval
include
