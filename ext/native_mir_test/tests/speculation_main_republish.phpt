--TEST--
Native: a specialized main script publishes a global once
--DESCRIPTION--
A global the specialized main script keeps in a register is published to
its slot at the first boundary after its definition and not again: a
callee that binds it by reference, or an output handler that changes it,
keeps its value through later boundaries and exits.
--EXTENSIONS--
opcache
--ENV--
ZEND_NATIVE_SPECULATE=1
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
// A callee binds a global by reference after the loop that computed it.
$acc = 1;
for ($ia = 0; $ia < 100; $ia++) { $acc = ($acc * 3 + ($ia & 7)) & 0xFFFF; }
echo $acc, "\n";
function bindref() { global $acc; $acc = "bound"; $GLOBALS['keep'] = &$acc; }
bindref();
$a = [1, 2];
for ($i = 0; $i < 3; $i++) { $v = @$a[$i + 5]; }
$f = 0.0;
for ($k = 1; $k <= 3; $k++) { $f += sqrt($k); }
var_dump($acc, $keep, $f);

// An output handler changes the global the script just printed.
ob_start(function ($buffer) { $GLOBALS['x'] = 'from handler'; return $buffer; }, 1);
$x = 0;
for ($j = 0; $j < 10; $j++) { $x += $j; }
echo $x, "\n";
ob_end_flush();
for ($m = 0; $m < 3; $m++) { $f += $m; }
var_dump($x);
?>
--EXPECT--
52371
string(5) "bound"
string(5) "bound"
float(4.146264369941973)
45
string(12) "from handler"
