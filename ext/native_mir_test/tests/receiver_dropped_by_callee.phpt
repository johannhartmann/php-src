--TEST--
A method that drops the last other reference to its receiver destroys it on return
--FILE--
<?php
class C {
    public $n;
    function __construct($n) { $this->n = $n; }
    function drop() { unset($GLOBALS['o']); return 1; }
    function nul() { $GLOBALS['o'] = null; return 2; }
    function __destruct() { echo "d{$this->n},"; }
}
for ($i = 0; $i < 4; $i++) { $o = new C($i); $o->drop(); echo "a$i,"; }
echo "\n";
for ($i = 0; $i < 4; $i++) { $o = new C($i + 10); $o->nul(); echo "b$i,"; }
echo "\n";
?>
--EXPECT--
d0,a0,d1,a1,d2,a2,d3,a3,
d10,b0,d11,b1,d12,b2,d13,b3,
