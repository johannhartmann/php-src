--TEST--
Native === and !== against literals through an EncodeGen snippet
--DESCRIPTION--
Identity of CVs, references and temporaries with literal null, bools,
integers and strings of every length inline; undefined variables, doubles,
arrays and objects take the helper.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function s($x) { return str_repeat($x, 1); }
function ids($v, $i)
{
    $alias = $v; $ref = &$alias;
    $built = s('post'); $long = s('abcdefghijklmnop'); $long2 = s('abcdefghijklmnoq');
    $out = [
        $v === 'post', $v !== 'post', $v === 'page', $v === 1, $v === null, $v === true,
        $v === false, $v === 0, $v === 1.5, $v === [], $v === [1], $ref === 'post',
        $built === 'post', $built === 'posts', $long === 'abcdefghijklmnop', $long2 === 'abcdefghijklmnop',
        s('x') === 'x', strpos("hay$i", 'y') === 3, $i + 1 === 2, $i === 0.0,
        s('') === '', $undefined === null,
    ];
    $flags = $v === 'post' ? 'P' : 'N';
    $out[] = $flags;
    return $out;
}
foreach (['post', 'page', 1, null, true, false, 0, 1.5, 0.0, -0.0, NAN, [], [1], new stdClass] as $i => $v) {
    echo json_encode(ids($v, $i), JSON_PARTIAL_OUTPUT_ON_ERROR), "\n";
}
?>
--EXPECTF--

Warning: Undefined variable $undefined in %s on line %d
[true,false,false,false,false,false,false,false,false,false,false,true,true,false,true,false,true,false,false,false,true,true,"P"]

Warning: Undefined variable $undefined in %s on line %d
[false,true,true,false,false,false,false,false,false,false,false,false,true,false,true,false,true,false,true,false,true,true,"N"]

Warning: Undefined variable $undefined in %s on line %d
[false,true,false,true,false,false,false,false,false,false,false,false,true,false,true,false,true,false,false,false,true,true,"N"]

Warning: Undefined variable $undefined in %s on line %d
[false,true,false,false,true,false,false,false,false,false,false,false,true,false,true,false,true,false,false,false,true,true,"N"]

Warning: Undefined variable $undefined in %s on line %d
[false,true,false,false,false,true,false,false,false,false,false,false,true,false,true,false,true,false,false,false,true,true,"N"]

Warning: Undefined variable $undefined in %s on line %d
[false,true,false,false,false,false,true,false,false,false,false,false,true,false,true,false,true,false,false,false,true,true,"N"]

Warning: Undefined variable $undefined in %s on line %d
[false,true,false,false,false,false,false,true,false,false,false,false,true,false,true,false,true,false,false,false,true,true,"N"]

Warning: Undefined variable $undefined in %s on line %d
[false,true,false,false,false,false,false,false,true,false,false,false,true,false,true,false,true,false,false,false,true,true,"N"]

Warning: Undefined variable $undefined in %s on line %d
[false,true,false,false,false,false,false,false,false,false,false,false,true,false,true,false,true,false,false,false,true,true,"N"]

Warning: Undefined variable $undefined in %s on line %d
[false,true,false,false,false,false,false,false,false,false,false,false,true,false,true,false,true,false,false,false,true,true,"N"]

Warning: Undefined variable $undefined in %s on line %d
[false,true,false,false,false,false,false,false,false,false,false,false,true,false,true,false,true,false,false,false,true,true,"N"]

Warning: Undefined variable $undefined in %s on line %d
[false,true,false,false,false,false,false,false,false,true,false,false,true,false,true,false,true,false,false,false,true,true,"N"]

Warning: Undefined variable $undefined in %s on line %d
[false,true,false,false,false,false,false,false,false,false,true,false,true,false,true,false,true,false,false,false,true,true,"N"]

Warning: Undefined variable $undefined in %s on line %d
[false,true,false,false,false,false,false,false,false,false,false,false,true,false,true,false,true,false,false,false,true,true,"N"]
