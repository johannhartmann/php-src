--TEST--
Native generator resumes unwind an exit from the body
--DESCRIPTION--
exit() in a directly resumed generator unwinds through the frame's bailout
record: as in the VM, shutdown functions and destructors run and the
generator's finally block does not.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class D { function __destruct() { echo "dtor\n"; } }
register_shutdown_function(function () { echo "shutdown\n"; });
function g() { $d = new D; try { yield 1; yield 2; exit("bye\n"); } finally { echo "finally\n"; } }
foreach (g() as $v) { echo $v, "\n"; }
?>
--EXPECT--
1
2
bye
dtor
shutdown
