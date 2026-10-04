--TEST--
Native frame cleanup that bails out still releases its activation
--DESCRIPTION--
Destructors that include their own file run natively during shutdown. When
freeing a native frame's CVs runs a nested destructor whose exception becomes
an uncaught fatal error, the bailout starts in frame cleanup after the native
entry returned. The frame must report that bailout so its callers release the
active call and the request can free every published native image.
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$file = __DIR__ . '/shutdown_destructor_bailout_release.inc';
file_put_contents($file, <<<'PHP'
<?php
if (!class_exists("C")) {
    class C {
        public function __destruct() {
            global $depth;
            if (++$depth > 2) {
                throw new Exception("stop at $depth");
            }
            $a = new C;
            require __FILE__;
        }
    }
}
$a = new C;
PHP);
require $file;
echo "done\n";
?>
--CLEAN--
<?php
@unlink(__DIR__ . '/shutdown_destructor_bailout_release.inc');
?>
--EXPECTF--
done

Fatal error: Uncaught Exception: stop at %d in %sshutdown_destructor_bailout_release.inc:7
Stack trace:
#0 [internal function]: C->__destruct()
#1 {main}
  thrown in %sshutdown_destructor_bailout_release.inc on line 7
