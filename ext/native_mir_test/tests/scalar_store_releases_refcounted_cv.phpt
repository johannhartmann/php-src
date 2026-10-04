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
final class NativeScalarStoreObject {
    public static int $live = 0;

    public function __construct() {
        self::$live++;
    }

    public function __destruct() {
        self::$live--;
    }
}

function native_scalar_store_literal_overwrites(): array {
    $live = [];

    $value = new NativeScalarStoreObject();
    $value = null;
    $live[] = NativeScalarStoreObject::$live;

    $value = new NativeScalarStoreObject();
    $value = false;
    $live[] = NativeScalarStoreObject::$live;

    $value = new NativeScalarStoreObject();
    $value = true;
    $live[] = NativeScalarStoreObject::$live;

    $value = new NativeScalarStoreObject();
    $value = 42;
    $live[] = NativeScalarStoreObject::$live;

    $value = new NativeScalarStoreObject();
    $value = 4.5;
    $live[] = NativeScalarStoreObject::$live;

    return $live;
}

function native_scalar_store_mixed_phi(bool $object): int {
    $value = $object ? new NativeScalarStoreObject() : 17;
    $value = 0;
    return NativeScalarStoreObject::$live;
}

function native_scalar_store_releases_refcounted_cv(): array {
    return [
        native_scalar_store_literal_overwrites(),
        native_scalar_store_mixed_phi(true),
        native_scalar_store_mixed_phi(false),
        NativeScalarStoreObject::$live,
    ];
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'scalar-store-releases-refcounted-cv.php',
    [],
    ['function' => 'native_scalar_store_releases_refcounted_cv'],
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
