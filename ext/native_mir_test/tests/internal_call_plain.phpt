--TEST--
Native plain internal call sites push and call without the general checks
--DESCRIPTION--
A direct internal call of a function without receiver or scope that is
neither deprecated, nodiscard nor a trampoline pushes its frame and calls
the handler through lean helpers. Used and unused results, exceptions
propagating through finally, named arguments that leave holes, and a
deprecated function on the general path behave as in the VM.
--EXTENSIONS--
native_mir_test
--FILE--
<?php
function run($n) {
    $out = [];
    for ($i = 0; $i < $n; $i++) {
        strlen("x$i");
        $out[] = str_repeat('ab', $i) . '|' . implode(',', array_keys(['a' => 1, "b$i" => 2]));
        try {
            try {
                json_decode('{', false, 512, JSON_THROW_ON_ERROR);
            } finally {
                $out[] = "finally $i";
            }
        } catch (JsonException $e) {
            $out[] = get_class($e) . ': ' . $e->getMessage();
        }
        $out[] = json_encode(value: [$i], flags: JSON_PRETTY_PRINT);
        $out[] = htmlspecialchars("<$i>", double_encode: false);
    }
    return implode("\n", $out);
}
echo run(2), "\n";
function deprecated_call() { return utf8_decode("abc"); }
var_dump(deprecated_call());
?>
--EXPECTF--
|a,b0
finally 0
JsonException: Syntax error near location 1:2
[
    0
]
&lt;0&gt;
ab|a,b1
finally 1
JsonException: Syntax error near location 1:2
[
    1
]
&lt;1&gt;

Deprecated: Function utf8_decode() is deprecated since 8.2, visit the php.net documentation for various alternatives in %s on line %d
string(3) "abc"
