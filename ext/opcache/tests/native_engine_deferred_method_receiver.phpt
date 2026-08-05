--TEST--
Native Engine resolves live method receivers in deferred OPcache bundles
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.jit=off
opcache.validate_timestamps=0
opcache.file_update_protection=0
--FILE--
<?php

interface NativeEngineDeferredMethodReceiverContract
{
}

class NativeEngineDeferredMethodReceiver implements NativeEngineDeferredMethodReceiverContract
{
    protected static int $base = 40;
    public int $value = 0;

    public function __construct()
    {
        for ($index = 0; $index < 10; $index++) {
            $this->setValue();
        }
    }

    private function setValue(): void
    {
        $this->value = static::$base + 2;
    }
}

$receiver = new NativeEngineDeferredMethodReceiver();
echo 'cached:', opcache_is_script_cached(__FILE__) ? 'yes' : 'no', "\n";
var_dump($receiver->value);
?>
--EXPECT--
cached:yes
int(42)
