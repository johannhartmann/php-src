--TEST--
Native TYPE_CHECK of CVs through an EncodeGen snippet
--DESCRIPTION--
is_*() and === null of CVs of every type, through references, as values and
as branch conditions, inline; undefined variables, which warn, and
is_resource(), which asks the resource list, take the helper.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function checks($v, $w)
{
    $alias = $v; $ref = &$alias;
    $out = [
        is_array($v), is_string($v), is_int($v), is_float($v), is_bool($v), is_null($v),
        $v === null, $v !== null, is_object($v), is_iterable($v), is_scalar($v),
        is_array($ref), is_string($ref), is_null($ref),
    ];
    if (is_array($v)) { $out[] = 'arr'; }
    if (!is_string($w)) { $out[] = 'notstr'; }
    $out[] = is_resource($w) ? 'res' : 'nores';
    $out[] = is_null($undefined);
    $out[] = $undefined === null;
    return $out;
}
$fh = fopen('php://memory', 'r');
foreach ([[1], 'str', 42, 1.5, true, null, new stdClass, $fh] as $i => $v) {
    echo json_encode(checks($v, $i % 2 ? $fh : 'x')), "\n";
}
fclose($fh);
echo json_encode(checks(1, $fh)), "\n";
?>
--EXPECTF--

Warning: Undefined variable $undefined in %s on line %d

Warning: Undefined variable $undefined in %s on line %d
[true,false,false,false,false,false,false,true,false,true,false,true,false,false,"arr","nores",true,true]

Warning: Undefined variable $undefined in %s on line %d

Warning: Undefined variable $undefined in %s on line %d
[false,true,false,false,false,false,false,true,false,false,true,false,true,false,"notstr","res",true,true]

Warning: Undefined variable $undefined in %s on line %d

Warning: Undefined variable $undefined in %s on line %d
[false,false,true,false,false,false,false,true,false,false,true,false,false,false,"nores",true,true]

Warning: Undefined variable $undefined in %s on line %d

Warning: Undefined variable $undefined in %s on line %d
[false,false,false,true,false,false,false,true,false,false,true,false,false,false,"notstr","res",true,true]

Warning: Undefined variable $undefined in %s on line %d

Warning: Undefined variable $undefined in %s on line %d
[false,false,false,false,true,false,false,true,false,false,true,false,false,false,"nores",true,true]

Warning: Undefined variable $undefined in %s on line %d

Warning: Undefined variable $undefined in %s on line %d
[false,false,false,false,false,true,true,false,false,false,false,false,false,true,"notstr","res",true,true]

Warning: Undefined variable $undefined in %s on line %d

Warning: Undefined variable $undefined in %s on line %d
[false,false,false,false,false,false,false,true,true,false,false,false,false,false,"nores",true,true]

Warning: Undefined variable $undefined in %s on line %d

Warning: Undefined variable $undefined in %s on line %d
[false,false,false,false,false,false,false,true,false,false,false,false,false,false,"notstr","res",true,true]

Warning: Undefined variable $undefined in %s on line %d

Warning: Undefined variable $undefined in %s on line %d
[false,false,true,false,false,false,false,true,false,false,true,false,false,false,"notstr","nores",true,true]
