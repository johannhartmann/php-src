--TEST--
Native method calls keep the receiver captured at INIT_METHOD_CALL
--DESCRIPTION--
The optimizer reuses a method receiver's temporary as soon as
INIT_METHOD_CALL has read it, and argument code may reassign a CV receiver.
A resolved native method call reads its receiver when it pushes the frame,
so it must only be used while that slot still holds the INIT receiver.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
class K {
    public function __construct(public string $n) {}
    public function m($x) {
        return $this->n . ':' . (is_object($x) ? get_class($x) : $x);
    }
}
function internal_temporary() {
    return (new ReflectionClass('K'))->newLazyGhost(function ($obj) {})
        instanceof K;
}
function user_temporary() {
    return (new K('tmp'))->m(function () {});
}
function internal_cv() {
    $o = new ArrayObject([1, 2, 3]);
    return $o->offsetExists(($o = new ArrayObject([9])) ? 2 : 0);
}
function user_cv() {
    $o = new K('first');
    return $o->m($o = new K('second'));
}
function internal_reference() {
    $o = new ArrayObject([1]);
    $r = &$o;
    return $o->offsetExists(($r = new ArrayObject([1, 2])) ? 1 : 0);
}
for ($i = 0; $i < 3; $i++) {
    var_dump(internal_temporary(), user_temporary(), internal_cv(),
        user_cv(), internal_reference());
}
?>
--EXPECT--
bool(true)
string(11) "tmp:Closure"
bool(true)
string(7) "first:K"
bool(false)
bool(true)
string(11) "tmp:Closure"
bool(true)
string(7) "first:K"
bool(false)
bool(true)
string(11) "tmp:Closure"
bool(true)
string(7) "first:K"
bool(false)
