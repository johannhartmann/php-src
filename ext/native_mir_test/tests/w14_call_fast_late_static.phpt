--TEST--
Native call-site fast path for static:: calls and private $this methods
--DESCRIPTION--
static:: calls take the fast path for the called scope a site was published
for and miss for another one; a private method called on $this takes it for
every subclass of the caller's class, including one that declares a public
method of the same name, while a closure keeps the universal protocol.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
require __DIR__ . '/w14_call_fast_late_static.inc';
for ($round = 0; $round < 3; $round++) {
    echo LBase::each(2), ' ', LChild::each(2), ' ', LOther::each(2), ' ', LChild::each(1), "\n";
    echo (new LBase)->viaStatic(), ' ', (new LChild)->viaStatic(), "\n";
    $b = new PBase; $c = new PChild; $s = new PShadow;
    echo $b->run(3), ' ', $c->run(2), ' ', $s->run(4), ' ', $s->shadowed, ' ', $b->run(1), "\n";
    $f = $c->bound();
    echo $f(), ' ', Closure::bind($f, $s, PBase::class)(), "\n";
}
?>
--EXPECT--
B:LBase0B:LBase1 C:LChild0C:LChild1 B:LOther0B:LOther1 C:LChild0
B:LBase C:LChild
PBase:3 PChild:2 PShadow:4 0 PBase:4
PChild:2 PShadow:4
B:LBase0B:LBase1 C:LChild0C:LChild1 B:LOther0B:LOther1 C:LChild0
B:LBase C:LChild
PBase:3 PChild:2 PShadow:4 0 PBase:4
PChild:2 PShadow:4
B:LBase0B:LBase1 C:LChild0C:LChild1 B:LOther0B:LOther1 C:LChild0
B:LBase C:LChild
PBase:3 PChild:2 PShadow:4 0 PBase:4
PChild:2 PShadow:4
