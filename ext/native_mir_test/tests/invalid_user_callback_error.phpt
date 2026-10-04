--TEST--
Native INIT_USER_CALL reports invalid callbacks as TypeError
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$result = native_mir_test_compile_execute(
    <<<'PHP'
<?php
abstract class NativeInvalidCallbackBase
{
    abstract public function target(): int;
}

final class NativeInvalidCallbackChild extends NativeInvalidCallbackBase
{
    public function target(): int
    {
        return 42;
    }
}

function native_invalid_user_callback_error(): int
{
    set_error_handler(static fn(): bool => true);
    try {
        $object = new NativeInvalidCallbackChild();
        call_user_func([
            $object,
            NativeInvalidCallbackBase::class . '::target',
        ]);
    } catch (TypeError) {
        return 1;
    } catch (Error) {
        return 2;
    } finally {
        restore_error_handler();
    }
    return 3;
}
PHP,
    'invalid-user-callback-error.php',
    [],
    [
        'function' => 'native_invalid_user_callback_error',
        'repeat' => 20,
    ],
);

printf(
    "%s return=%d runs=%d vm=%d active=%d\n",
    $result['status'],
    $result['execution']['return_value'],
    $result['execution']['executions'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=1 runs=20 vm=0 active=0
