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
abstract class W11PInvalidCallbackBase
{
    abstract public function target(): int;
}

final class W11PInvalidCallbackChild extends W11PInvalidCallbackBase
{
    public function target(): int
    {
        return 42;
    }
}

function w11p_invalid_user_callback_error(): int
{
    set_error_handler(static fn(): bool => true);
    try {
        $object = new W11PInvalidCallbackChild();
        call_user_func([
            $object,
            W11PInvalidCallbackBase::class . '::target',
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
    'w11p-invalid-user-callback-error.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_invalid_user_callback_error',
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
