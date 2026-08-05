--TEST--
Native scalar stores release refcounted CV values
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$source = <<<'PHP'
<?php
final class W11PScalarStoreObject {
    public static int $live = 0;

    public function __construct() {
        self::$live++;
    }

    public function __destruct() {
        self::$live--;
    }
}

function w11p_scalar_store_literal_overwrites(): array {
    $live = [];

    $value = new W11PScalarStoreObject();
    $value = null;
    $live[] = W11PScalarStoreObject::$live;

    $value = new W11PScalarStoreObject();
    $value = false;
    $live[] = W11PScalarStoreObject::$live;

    $value = new W11PScalarStoreObject();
    $value = true;
    $live[] = W11PScalarStoreObject::$live;

    $value = new W11PScalarStoreObject();
    $value = 42;
    $live[] = W11PScalarStoreObject::$live;

    $value = new W11PScalarStoreObject();
    $value = 4.5;
    $live[] = W11PScalarStoreObject::$live;

    return $live;
}

function w11p_scalar_store_mixed_phi(bool $object): int {
    $value = $object ? new W11PScalarStoreObject() : 17;
    $value = 0;
    return W11PScalarStoreObject::$live;
}

function w11p_scalar_store_releases_refcounted_cv(): array {
    return [
        w11p_scalar_store_literal_overwrites(),
        w11p_scalar_store_mixed_phi(true),
        w11p_scalar_store_mixed_phi(false),
        W11PScalarStoreObject::$live,
    ];
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-scalar-store-releases-refcounted-cv.php',
    [],
    ['wave' => 11, 'function' => 'w11p_scalar_store_releases_refcounted_cv'],
);

printf(
    "%s return=%s vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value'] ?? null),
    $result['execution']['vm_handler_calls'] ?? -1,
    $result['execution']['execute_ex_calls'] ?? -1,
    $result['execution']['opline_handler_calls'] ?? -1,
);
?>
--EXPECT--
accepted return=[[0,0,0,0,0],0,0,0] vm=0 execute_ex=0 handler=0
