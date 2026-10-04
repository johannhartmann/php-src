--TEST--
Native recorded dynamic call targets survive the call-cache epoch
--DESCRIPTION--
A recorded target of call_user_func*() is current again in a later epoch,
as after a request, when its function and class are immutable, its names
are permanent interned strings, its entry cell still holds the recorded
entry and a function or class name still binds it. Closures and other
targets resolve again.
--EXTENSIONS--
opcache
native_mir_test
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function plain($a, $b = 'd') { return "plain($a,$b)"; }
class H { public $p = 'h'; function m($a) { return "m($this->p,$a)"; } static function sm($a) { return static::class . "::sm($a)"; } }
class G extends H {}
function invalidate() { if (function_exists('native_mir_test_call_cache_invalidate')) { native_mir_test_call_cache_invalidate(); } }
function run() {
    $h = new H; $g = new G; $g->p = 'g';
    $callbacks = ['plain', 'PLAIN', [$h, 'm'], [$g, 'm'], ['H', 'sm'], ['G', 'sm'], fn($a) => "closure($a)"];
    $out = [];
    for ($round = 0; $round < 3; $round++) {
        foreach ($callbacks as $i => $cb) {
            $out[] = call_user_func_array($cb, ["r$round"]) . ' ' . call_user_func($cb, $i);
        }
        invalidate();
    }
    return implode("\n", array_unique($out));
}
echo run(), "\n";
?>
--EXPECT--
plain(r0,d) plain(0,d)
plain(r0,d) plain(1,d)
m(h,r0) m(h,2)
m(g,r0) m(g,3)
H::sm(r0) H::sm(4)
G::sm(r0) G::sm(5)
closure(r0) closure(6)
plain(r1,d) plain(0,d)
plain(r1,d) plain(1,d)
m(h,r1) m(h,2)
m(g,r1) m(g,3)
H::sm(r1) H::sm(4)
G::sm(r1) G::sm(5)
closure(r1) closure(6)
plain(r2,d) plain(0,d)
plain(r2,d) plain(1,d)
m(h,r2) m(h,2)
m(g,r2) m(g,3)
H::sm(r2) H::sm(4)
G::sm(r2) G::sm(5)
closure(r2) closure(6)
