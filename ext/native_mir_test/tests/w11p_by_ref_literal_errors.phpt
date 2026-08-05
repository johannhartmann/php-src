--TEST--
Native calls preserve by-reference literal errors
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$cases = [
    'user' => [
        <<<'PHP'
<?php
function w11p_by_ref_target(&$value): void {}
function w11p_by_ref_user_literal(): void
{
    w11p_by_ref_target(2);
}
PHP,
        'w11p_by_ref_user_literal',
    ],
    'internal' => [
        <<<'PHP'
<?php
function w11p_by_ref_internal_literal(): void
{
    preg_match_all('/x/', 'x', []);
}
PHP,
        'w11p_by_ref_internal_literal',
    ],
];

foreach ($cases as $name => [$source, $function]) {
    try {
        native_mir_test_compile_execute(
            $source,
            "w11p-by-ref-$name-literal.php",
            [],
            [
                'wave' => 11,
                'function' => $function,
            ],
        );
        echo "$name missing\n";
    } catch (Error $error) {
        printf("%s: %s\n", $name, $error->getMessage());
    }
}
?>
--EXPECT--
user: w11p_by_ref_target(): Argument #1 ($value) could not be passed by reference
internal: preg_match_all(): Argument #3 ($matches) could not be passed by reference
