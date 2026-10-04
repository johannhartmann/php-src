--TEST--
Native === and !== of scalars and strings without decoding
--DESCRIPTION--
=== and !== of scalars, strings and values of different types decide at
the helper's entry, releasing string temporaries before publishing the
result. Arrays, objects, NAN, references and undefined variables keep
stock semantics.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function s($x) { return $x; }
function t($r) {
  $a = 'abc'; $b = s('ab') . 'c'; $n = null; $arr = [1]; $o = new stdClass; $f = 1.0; $ref = &$a;
  return [s('x') === 'x', s('x') !== 'x', $a === $b, $b === s('abc'), s(1) === 1, s(1) === '1', s(1.0) === 1, $n === null, s(null) !== null,
    $arr === [1], s([1]) === [1], $o === $o, s($o) === $o, $f === 1.0, s(NAN) === NAN, $ref === 'abc', s(true) === true, s('') === false,
    @$undef === null, str_repeat('z', 3) === 'zzz'];
}
for ($i = 0; $i < 2; $i++) echo json_encode(t($i)), "\n";
?>
--EXPECT--
[true,false,true,true,true,false,false,true,false,true,true,true,true,true,false,true,true,false,true,true]
[true,false,true,true,true,false,false,true,false,true,true,true,true,true,false,true,true,false,true,true]
