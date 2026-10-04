--TEST--
Native call-site fast path for static and parent:: method calls
--DESCRIPTION--
Static method calls of a named class and self::/parent:: calls, including
instance methods called through parent:: with $this, take the call-site
fast path; late static binding, the forwarded called scope and $this match
stock PHP.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
require __DIR__ . '/call_fast_static.inc';
function run($r) {
    $c = new SChild; $b = new SBase;
    return [SBase::who($r), SChild::who($r), SBase::viaSelf($r), SChild::viaSelf($r),
        SChild::viaNamed($r), SChild::viaParent($r), $c->instViaParent($r),
        $b->instViaSelf($r), $c->instViaSelf($r), get_class(SChild::make()),
        $c->callTyped($r), SBase::who()];
}
for ($i = 0; $i < 3; $i++) { echo json_encode(run($i)), "\n"; }
try { SChild::inst(1); } catch (Error $e) { echo get_class($e), "\n"; }
?>
--EXPECT--
["SBase0","SChild0","SBase0","SChild0","SBase0","SChild0","SChild:2|SChild0","SBase0|SBase:1","SChild0|SChild:2","SChild",2,"SBase0"]
["SBase1","SChild1","SBase1","SChild1","SBase1","SChild1","SChild:3|SChild1","SBase1|SBase:2","SChild1|SChild:3","SChild",3,"SBase0"]
["SBase2","SChild2","SBase2","SChild2","SBase2","SChild2","SChild:4|SChild2","SBase2|SBase:3","SChild2|SChild:4","SChild",4,"SBase0"]
Error
