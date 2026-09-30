--TEST--
Native INSTANCEOF through the run-time cached class
--DESCRIPTION--
$x instanceof Literal reads the class the run-time cache keeps, as the VM
does: exact classes, subclasses, interfaces, non-objects, references,
undefined variables and classes declared later keep stock semantics.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
interface Shape {}
class Base implements Shape {}
class Child extends Base {}
function test($v) {
    $r = &$v;
    return [$v instanceof Base, $v instanceof Child, $v instanceof Shape, $v instanceof Later, $r instanceof Base];
}
function undefined_cv() { return $missing instanceof Base; }
$out = [];
foreach ([new Base, new Child, null, 1, 'Base', [], new stdClass] as $v) { $out[] = test($v); }
echo json_encode($out), "\n";
var_dump(undefined_cv());
class Later {}
echo json_encode(test(new Later)), "\n";
?>
--EXPECTF--
[[true,false,true,false,true],[true,true,true,false,true],[false,false,false,false,false],[false,false,false,false,false],[false,false,false,false,false],[false,false,false,false,false],[false,false,false,false,false]]

Warning: Undefined variable $missing in %s on line %d
bool(false)
[false,false,false,true,false]
