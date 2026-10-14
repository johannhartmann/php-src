--TEST--
Native cached property write of an undefined variable
--DESCRIPTION--
An inlined property write stores a CV only when it is defined: an undefined
one warns and stores null on every call, as the VM does.
--EXTENSIONS--
native_mir_test
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class C {
    public $p = 1;
    function set() { $this->p = $u; return $this->p; }
}
function f($o) { $o->p = $undef; return $o->p; }
$o = new C;
$warnings = 0;
set_error_handler(function ($no, $msg) use (&$warnings) {
    if ($msg !== 'Undefined variable $undef' && $msg !== 'Undefined variable $u') {
        echo $msg, "\n";
    }
    $warnings++;
    return true;
});
for ($i = 0; $i < 50; $i++) { $r = f($o); $s = $o->set(); }
var_dump($r, $s, $warnings);
?>
--EXPECT--
NULL
NULL
int(100)
