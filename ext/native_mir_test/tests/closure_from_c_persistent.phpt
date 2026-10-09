--TEST--
Native: a closure of a cached script entered from C runs from its persistent generation
--DESCRIPTION--
A shutdown function or an error handler closure enters the engine from C
with a mutable closure op_array copy; it is compiled in the script's
persistent generation, like a direct call of the same closure.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
$file = __DIR__ . '/closure_from_c_persistent.inc';
file_put_contents($file, <<<'PHP'
<?php
class NativeShutdownForwarder {
    public static $app = true;
    public function boot() {
        register_shutdown_function($this->forwardsTo('handleShutdown'));
        set_error_handler($this->forwardsTo('handleError'));
        return $this->forwardsTo('direct');
    }
    protected function forwardsTo($method) {
        return fn (...$arguments) => static::$app ? $this->{$method}(...$arguments) : false;
    }
    public function handleShutdown() { echo "shutdown ", strlen('abc'), "\n"; }
    public function handleError($level, $message) { echo "error: $message\n"; return true; }
    public function direct($x) { return $x * 2; }
}
PHP);
require $file;
$f = (new NativeShutdownForwarder)->boot();
echo $f(21), "\n";
echo $undefined;
unlink($file);
?>
--EXPECT--
42
error: Undefined variable $undefined
shutdown 3
