--TEST--
Native fast call frames receive missing parameters from their defaults
--DESCRIPTION--
Dynamic call sites (call_user_func(), $f(), [$object, 'method']) and sites
whose frame needs a prepare (extra arguments, a variadic target) receive
missing parameters inline only when every default from the first missing
parameter on is a literal; constant-expression defaults, required
parameters and typed variadics take the generic receive.
--EXTENSIONS--
opcache
native_mir_test
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
const LIMIT = 7;

function literal_tail($a, $b = 2, $c = 'c') {
    return "$a|$b|$c";
}

function ast_middle($a, $b = LIMIT * 2, $c = 3) {
    return "$a|$b|$c";
}

function ast_last($a, $b = 2, $c = LIMIT) {
    return "$a|$b|$c";
}

function required_middle($a, $b, $c = 3) {
    return "$a|$b|$c";
}

function untyped_variadic($a, $b = 'b', ...$rest) {
    return "$a|$b|" . implode(',', $rest);
}

function typed_variadic(int $a, int $b = 2, int ...$rest) {
    return "$a|$b|" . implode(',', $rest);
}

function pick_name() {
    return 'literal_tail';
}

class Box {
    public function pick($a, $b = null, $c = [1]) {
        return $a . '|' . var_export($b, true) . '|' . count($c);
    }
}

function run(): void {
    $box = new Box;
    $names = ['literal_tail', 'ast_middle', 'ast_last', 'untyped_variadic',
        'typed_variadic'];
    for ($i = 0; $i < 3; $i++) {
        foreach ($names as $name) {
            echo call_user_func($name, 1), "\n";
            echo call_user_func($name, 1, 5), "\n";
            echo $name(1, 5, 6), "\n";
            echo $name(1, 5, 6, 7), "\n";
        }
        echo call_user_func([$box, 'pick'], 'x'), "\n";
        echo call_user_func([$box, 'pick'], 'x', false), "\n";
        $f = 'required_middle';
        echo $f(1, 2), "\n";
        try {
            echo $f(1), "\n";
        } catch (ArgumentCountError $e) {
            echo get_class($e), ': ', $e->getMessage(), "\n";
        }
        try {
            echo call_user_func('typed_variadic', 1, 2, 'x'), "\n";
        } catch (TypeError $e) {
            echo get_class($e), ': ', $e->getMessage(), "\n";
        }
        echo (pick_name())(1), "\n";
        echo call_user_func(pick_name(), 1, 5), "\n";
        echo (fn($a, $b = 'd') => "$a|$b")(1), "\n";
    }
}

run();
?>
--EXPECTF--
1|2|c
1|5|c
1|5|6
1|5|6
1|14|3
1|5|3
1|5|6
1|5|6
1|2|7
1|5|7
1|5|6
1|5|6
1|b|
1|5|
1|5|6
1|5|6,7
1|2|
1|5|
1|5|6
1|5|6,7
x|NULL|1
x|false|1
1|2|3
ArgumentCountError: Too few arguments to function required_middle(), 1 passed in %s on line %d and at least 2 expected
TypeError: typed_variadic(): Argument #3 must be of type int, string given, called in %s on line %d
1|2|c
1|5|c
1|d
1|2|c
1|5|c
1|5|6
1|5|6
1|14|3
1|5|3
1|5|6
1|5|6
1|2|7
1|5|7
1|5|6
1|5|6
1|b|
1|5|
1|5|6
1|5|6,7
1|2|
1|5|
1|5|6
1|5|6,7
x|NULL|1
x|false|1
1|2|3
ArgumentCountError: Too few arguments to function required_middle(), 1 passed in %s on line %d and at least 2 expected
TypeError: typed_variadic(): Argument #3 must be of type int, string given, called in %s on line %d
1|2|c
1|5|c
1|d
1|2|c
1|5|c
1|5|6
1|5|6
1|14|3
1|5|3
1|5|6
1|5|6
1|2|7
1|5|7
1|5|6
1|5|6
1|b|
1|5|
1|5|6
1|5|6,7
1|2|
1|5|
1|5|6
1|5|6,7
x|NULL|1
x|false|1
1|2|3
ArgumentCountError: Too few arguments to function required_middle(), 1 passed in %s on line %d and at least 2 expected
TypeError: typed_variadic(): Argument #3 must be of type int, string given, called in %s on line %d
1|2|c
1|5|c
1|d
