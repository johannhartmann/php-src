--TEST--
Native: a call whose argument always throws discards its INIT_FCALL
--DESCRIPTION--
The optimizer folds a match without a matching arm into MATCH_ERROR, which
leaves INIT_FCALL without its DO; the call never runs and is discarded as
one an expression throw ends, instead of rejecting the function.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function always($v) { return strlen(match(true) { 1, 2 => 'x' }); }
function some($v) { return strlen(match($v) { 1 => 'one', 2 => 'two' }); }
function before($v) { return sprintf('%s-%s', 'a', match(true) { 1 => 'b' }); }
foreach ([fn() => always(1), fn() => some(1), fn() => some(2), fn() => some(3), fn() => before(1)] as $f) {
    try { var_dump($f()); } catch (\UnhandledMatchError $e) { echo get_class($e), ': ', $e->getMessage(), "\n"; }
}
try {
    var_dump(match(true) { 1, 2, 3 => 'foo' });
} catch (Error $e) {
    echo $e->getMessage(), "\n";
}
?>
--EXPECT--
UnhandledMatchError: Unhandled match case true
int(3)
int(3)
UnhandledMatchError: Unhandled match case 3
UnhandledMatchError: Unhandled match case true
Unhandled match case true
