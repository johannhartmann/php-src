--TEST--
Native frame leave releases CVs before extra arguments
--DESCRIPTION--
Like the VM's leave helper, a returning frame destroys its compiled variables
first and its extra arguments afterwards, on the fast and the general path.
--EXTENSIONS--
native_mir_test
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class D {
    function __construct(public $n) {}
    function __destruct() { echo $this->n, " "; }
}
function f($a) { $x = new D('cv'); return 1; }
function g($a) { $x = new D('cv'); }
function h($a) { $x = new D('cv'); $n = 'x'; return isset($$n); }
function k($a) { $x = new D('cv'); throw new Exception('e'); }
for ($i = 0; $i < 50; $i++) {
    f(1, new D('extra'));
    echo "| ";
    g(1, new D('extra2'));
    echo "| ";
    h(1, new D('extra3'));
    echo "| ";
    try { k(1, new D('extra4')); } catch (Exception $e) {}
    echo "\n";
}
?>
--EXPECT--
cv extra | cv extra2 | cv extra3 | cv 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
cv extra | cv extra2 | cv extra3 | cv extra4 
extra4 