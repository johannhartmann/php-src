--TEST--
Native FETCH_STATIC_PROP_R through the run-time cache
--DESCRIPTION--
Static property reads of a literal name with a literal class, self:: or
parent:: read the property the run-time cache keeps, as the VM does;
static::, typed uninitialized, private and protected properties and
references keep stock semantics.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class SP { public static $a = 1; public static array $list = [1]; public static ?int $typed; protected static $prot = 'p'; private static $priv = 'x';
  static function read() { return [self::$a, static::$a, self::$list, SP::$a, self::$prot, self::$priv]; }
  static function bump() { self::$a++; self::$list[] = self::$a; } }
class SPC extends SP { public static $a = 'child'; static function r2() { return [parent::$a, self::$a, static::$a, parent::$prot]; } }
$out = [];
for ($i = 0; $i < 3; $i++) { SP::bump(); $out[] = [SP::read(), SPC::read(), SPC::r2(), SP::$a]; }
try { echo SP::$typed; } catch (Error $e) { $out[] = get_class($e); }
try { echo SP::$priv; } catch (Error $e) { $out[] = get_class($e); }
$ref = &SP::$a; $ref = 'ref'; $out[] = SP::read();
echo json_encode($out), "\n";
?>
--EXPECT--
[[[2,2,[1,2],2,"p","x"],[2,"child",[1,2],2,"p","x"],[2,"child","child","p"],2],[[3,3,[1,2,3],3,"p","x"],[3,"child",[1,2,3],3,"p","x"],[3,"child","child","p"],3],[[4,4,[1,2,3,4],4,"p","x"],[4,"child",[1,2,3,4],4,"p","x"],[4,"child","child","p"],4],"Error","Error",["ref","ref",[1,2,3,4],"ref","p","x"]]
