--TEST--
Native MATCH and SWITCH execute direct multiway branches without VM dispatch
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
function w12_long_switch(mixed $value): string
{
    switch ($value) {
        case -4: return "negative";
        case 0: return "zero";
        case 1: return "one";
        case 2: return "two";
        case 3: return "three";
        case 4: return "four";
        case 5: return "five";
        case 19: return "nineteen";
        default: return "other";
    }
}
function w12_string_switch(mixed $value): string
{
    switch ($value) {
        case "alpha": return "a";
        case "bravo-long": return "b";
        case "charlie": return "c";
        default: return "other";
    }
}
function w12_match(mixed $value): string
{
    return match ($value) {
        -7 => "negative",
        5 => "five",
        "five" => "string-five",
        "long-string-arm" => "long",
        default => "other",
    };
}
function w12_case_fallback(mixed $left, mixed $right): string
{
    switch ($left . $right) {
        case 99: return "wrong";
        case 12: return "loose";
        default: return "other";
    }
}
function w12_case_strict_fallback(mixed $left, mixed $right): string
{
    $numeric = 12;
    $string = "12";
    return match ($left . $right) {
        $numeric => "numeric",
        $string => "strict",
        default => "other",
    };
}
PHP;

$cases = [
    ['w12_long_switch', [-4], 'negative', 'ZEND_SWITCH_LONG'],
    ['w12_long_switch', [4], 'four', 'ZEND_SWITCH_LONG'],
    ['w12_long_switch', [19], 'nineteen', 'ZEND_SWITCH_LONG'],
    ['w12_long_switch', [8], 'other', 'ZEND_SWITCH_LONG'],
    ['w12_long_switch', ['4'], 'four', 'ZEND_SWITCH_LONG'],
    ['w12_string_switch', ['bravo-' . 'long'], 'b', 'ZEND_SWITCH_STRING'],
    ['w12_string_switch', ['missing'], 'other', 'ZEND_SWITCH_STRING'],
    ['w12_string_switch', [true], 'a', 'ZEND_SWITCH_STRING'],
    ['w12_match', [-7], 'negative', 'ZEND_MATCH'],
    ['w12_match', [5], 'five', 'ZEND_MATCH'],
    ['w12_match', ['fi' . 've'], 'string-five', 'ZEND_MATCH'],
    ['w12_match', [true], 'other', 'ZEND_MATCH'],
    ['w12_case_fallback', [1, 2], 'loose', 'ZEND_CASE'],
    ['w12_case_strict_fallback', [1, 2], 'strict', 'ZEND_CASE_STRICT'],
];
foreach ($cases as [$function, $arguments, $expected, $expectedOpcode]) {
    $result = native_mir_test_compile_execute(
        $source,
        'w12-multiway.php',
        $arguments,
        ['wave' => 11, 'function' => $function],
    );
    printf(
        "%s(%s) status=%s opcode=%s result=%s expected=%s vm=%d execute_ex=%d handler=%d\n",
        $function,
        json_encode(count($arguments) == 1 ? $arguments[0] : $arguments),
        $result['status'],
        in_array($expectedOpcode, $result['source_opcodes'], true)
            ? $expectedOpcode : 'missing',
        json_encode($result['execution']['return_value'] ?? null),
        json_encode($expected),
        $result['execution']['vm_handler_calls'] ?? -1,
        $result['execution']['execute_ex_calls'] ?? -1,
        $result['execution']['opline_handler_calls'] ?? -1,
    );
    if ($result['status'] !== 'accepted') {
        printf("diagnostics=%s\n", json_encode($result['diagnostics'] ?? null));
    }
}
?>
--EXPECT--
w12_long_switch(-4) status=accepted opcode=ZEND_SWITCH_LONG result="negative" expected="negative" vm=0 execute_ex=0 handler=0
w12_long_switch(4) status=accepted opcode=ZEND_SWITCH_LONG result="four" expected="four" vm=0 execute_ex=0 handler=0
w12_long_switch(19) status=accepted opcode=ZEND_SWITCH_LONG result="nineteen" expected="nineteen" vm=0 execute_ex=0 handler=0
w12_long_switch(8) status=accepted opcode=ZEND_SWITCH_LONG result="other" expected="other" vm=0 execute_ex=0 handler=0
w12_long_switch("4") status=accepted opcode=ZEND_SWITCH_LONG result="four" expected="four" vm=0 execute_ex=0 handler=0
w12_string_switch("bravo-long") status=accepted opcode=ZEND_SWITCH_STRING result="b" expected="b" vm=0 execute_ex=0 handler=0
w12_string_switch("missing") status=accepted opcode=ZEND_SWITCH_STRING result="other" expected="other" vm=0 execute_ex=0 handler=0
w12_string_switch(true) status=accepted opcode=ZEND_SWITCH_STRING result="a" expected="a" vm=0 execute_ex=0 handler=0
w12_match(-7) status=accepted opcode=ZEND_MATCH result="negative" expected="negative" vm=0 execute_ex=0 handler=0
w12_match(5) status=accepted opcode=ZEND_MATCH result="five" expected="five" vm=0 execute_ex=0 handler=0
w12_match("five") status=accepted opcode=ZEND_MATCH result="string-five" expected="string-five" vm=0 execute_ex=0 handler=0
w12_match(true) status=accepted opcode=ZEND_MATCH result="other" expected="other" vm=0 execute_ex=0 handler=0
w12_case_fallback([1,2]) status=accepted opcode=ZEND_CASE result="loose" expected="loose" vm=0 execute_ex=0 handler=0
w12_case_strict_fallback([1,2]) status=accepted opcode=ZEND_CASE_STRICT result="strict" expected="strict" vm=0 execute_ex=0 handler=0
