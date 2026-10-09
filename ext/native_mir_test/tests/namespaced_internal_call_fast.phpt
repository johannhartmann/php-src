--TEST--
Native: namespaced calls of internal functions take the fast call path with VM semantics
--DESCRIPTION--
An unqualified call of an internal function in a namespace binds by name
at run time (INIT_NS_FCALL_BY_NAME). Its site is published for the fast
path once resolved: the Do runs the handler as ZEND_DO_ICALL does. Errors,
warnings and their lines, by-reference parameters, callbacks with
backtraces, frame-inspecting functions and a namespaced function declared
after the first call behave as in the VM.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
namespace App\Util;

function lengths(array $items): array {
    $out = [];
    foreach ($items as $item) {
        $out[] = strlen($item) + count(explode(',', $item));
    }
    return $out;
}

function warn(array $values) {
    $r = [];
    foreach ($values as $v) {
        try {
            $r[] = array_combine($v, [1]);
        } catch (\ValueError $e) {
            $r[] = 'ValueError line ' . $e->getLine();
        }
        $r[] = @hex2bin('abc');
        $r[] = intdiv(10, max(1, (int) ($v[0] ?? 1)));
    }
    return $r;
}

function typed(array $values) {
    $r = [];
    foreach ($values as $v) {
        try {
            $r[] = strlen($v);
        } catch (\TypeError $e) {
            $r[] = get_class($e) . ' line ' . $e->getLine() . ': ' . $e->getMessage();
        }
    }
    return $r;
}

function byref(array $subjects) {
    $r = [];
    foreach ($subjects as $s) {
        $m = null;
        $n = preg_match('/(\d+)-(\d+)/', $s, $m);
        $parts = explode('-', $s);
        sort($parts);
        $r[] = [$n, $m, $parts];
    }
    return $r;
}

function callbacks(array $values) {
    return array_map(function ($v) {
        $t = debug_backtrace(\DEBUG_BACKTRACE_IGNORE_ARGS);
        return $v * 2 . '@' . ($t[1]['function'] ?? '-') . ':' . ($t[0]['line'] ?? '-');
    }, $values);
}

function args() {
    $a = func_get_args();
    $x = 1; $y = 2;
    $c = compact('x', 'y');
    extract(['z' => 3]);
    return [$a, $c, $z, func_num_args()];
}

function warning_line($n) {
    $r = [];
    for ($i = 0; $i < $n; $i++) {
        $r[] = str_repeat('a', $i % 2 ? -1 : 1);
    }
    return $r;
}

function shadow() {
    $r = [];
    for ($i = 0; $i < 3; $i++) {
        $r[] = strtoupper('x');
        if ($i === 0) {
            eval('namespace App\Util; function strtoupper($s) { return "ns:" . $s; }');
        }
    }
    return $r;
}

for ($round = 0; $round < 3; $round++) {
    echo json_encode(lengths(['a,b', 'abc', ''])), "\n";
    echo json_encode(warn([[1], ['a', 'b'], [3]])), "\n";
    echo json_encode(typed(['abc', [], 'de'])), "\n";
    echo json_encode(byref(['10-20', 'x', '3-1'])), "\n";
    echo json_encode(callbacks([1, 2])), "\n";
    echo json_encode(args(1, 'two')), "\n";
    try {
        echo json_encode(warning_line(2)), "\n";
    } catch (\ValueError $e) {
        echo 'ValueError line ', $e->getLine(), ': ', $e->getMessage(), "\n";
    }
}
echo json_encode(shadow()), "\n";
echo json_encode(strtoupper('y')), "\n";
--EXPECTF--
[5,4,1]
[{"1":1},false,10,"ValueError line 16",false,10,{"3":1},false,3]
[3,"TypeError line 30: strlen(): Argument #1 ($string) must be of type string, array given",2]
[[1,["10-20","10","20"],["10","20"]],[0,[],["x"]],[1,["3-1","3","1"],["1","3"]]]
["2@array_map:-","4@array_map:-"]
[[1,"two"],{"x":1,"y":2},3,2]
ValueError line 68: str_repeat(): Argument #2 ($times) must be greater than or equal to 0
[5,4,1]
[{"1":1},false,10,"ValueError line 16",false,10,{"3":1},false,3]
[3,"TypeError line 30: strlen(): Argument #1 ($string) must be of type string, array given",2]
[[1,["10-20","10","20"],["10","20"]],[0,[],["x"]],[1,["3-1","3","1"],["1","3"]]]
["2@array_map:-","4@array_map:-"]
[[1,"two"],{"x":1,"y":2},3,2]
ValueError line 68: str_repeat(): Argument #2 ($times) must be greater than or equal to 0
[5,4,1]
[{"1":1},false,10,"ValueError line 16",false,10,{"3":1},false,3]
[3,"TypeError line 30: strlen(): Argument #1 ($string) must be of type string, array given",2]
[[1,["10-20","10","20"],["10","20"]],[0,[],["x"]],[1,["3-1","3","1"],["1","3"]]]
["2@array_map:-","4@array_map:-"]
[[1,"two"],{"x":1,"y":2},3,2]
ValueError line 68: str_repeat(): Argument #2 ($times) must be greater than or equal to 0
["X","X","X"]
"ns:y"
