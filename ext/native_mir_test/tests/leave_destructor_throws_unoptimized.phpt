--TEST--
Native: a destructor that throws when a call leaves fails the call (unoptimized)
--DESCRIPTION--
A destructor of a CV, an extra argument, $this or the closure that throws
while the callee leaves fails the call, as the VM's leave reports it: the
returned result dies instead of being used, and nothing leaks.
--INI--
opcache.enable_cli=0
--FILE--
<?php
class Bad {
    public function __destruct() { throw new Exception('dtor'); }
    public function loop() { $s = ''; for ($i = 0; $i < 2; $i++) { $s .= 'b'; } return $s; }
    public function plain() { return str_repeat('p', 2); }
}
function arg_loop($o) { $s = ''; for ($i = 0; $i < 2; $i++) { $s .= 'a'; } return $s; }
function arg_plain($o) { return str_repeat('q', 2); }
function arg_array($o) { $n = 0; for ($i = 0; $i < 2; $i++) { $n += 5; } return [$n, str_repeat('r', 2)]; }
function extra() { $s = ''; for ($i = 0; $i < 2; $i++) { $s .= 'e'; } return $s; }
$cases = [
    'this loop' => fn() => (new Bad)->loop(),
    'this plain' => fn() => (new Bad)->plain(),
    'arg loop' => fn() => arg_loop(new Bad),
    'arg plain' => fn() => arg_plain(new Bad),
    'arg array' => fn() => arg_array(new Bad),
    'extra' => fn() => extra(1, new Bad),
    'closure' => function () {
        $b = new Bad;
        $f = function () use ($b) { $s = ''; for ($i = 0; $i < 2; $i++) { $s .= 'c'; } return $s; };
        unset($b);
        return (function ($f) { $x = $f(); return $x; })($f);
    },
];
function consume($f) { try { $x = $f(); echo 'x=', json_encode($x), "\n"; } catch (Exception $e) { echo 'caught ', $e->getMessage(), "\n"; } }
function consume_echo($f) { try { echo strlen($f()), "\n"; } catch (Exception $e) { echo 'caught ', $e->getMessage(), "\n"; } }
for ($round = 0; $round < 3; $round++) {
    foreach ($cases as $name => $f) {
        echo "$name: ";
        consume($f);
        echo "$name echo: ";
        consume_echo($f);
    }
}
?>
--EXPECT--
this loop: caught dtor
this loop echo: caught dtor
this plain: caught dtor
this plain echo: caught dtor
arg loop: caught dtor
arg loop echo: caught dtor
arg plain: caught dtor
arg plain echo: caught dtor
arg array: caught dtor
arg array echo: caught dtor
extra: caught dtor
extra echo: caught dtor
closure: caught dtor
closure echo: caught dtor
this loop: caught dtor
this loop echo: caught dtor
this plain: caught dtor
this plain echo: caught dtor
arg loop: caught dtor
arg loop echo: caught dtor
arg plain: caught dtor
arg plain echo: caught dtor
arg array: caught dtor
arg array echo: caught dtor
extra: caught dtor
extra echo: caught dtor
closure: caught dtor
closure echo: caught dtor
this loop: caught dtor
this loop echo: caught dtor
this plain: caught dtor
this plain echo: caught dtor
arg loop: caught dtor
arg loop echo: caught dtor
arg plain: caught dtor
arg plain echo: caught dtor
arg array: caught dtor
arg array echo: caught dtor
extra: caught dtor
extra echo: caught dtor
closure: caught dtor
closure echo: caught dtor
