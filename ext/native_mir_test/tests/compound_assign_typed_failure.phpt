--TEST--
Native compound assignment to a typed property or reference whose operation throws
--DESCRIPTION--
A failed operation leaves the coerced undefined copy that the VM's typed-property path assigns.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class C { public int $x = 5; public static int $s = 6; public $u = 1; }
function f() {
  $o = new C;
  try { $o->x += []; } catch (Throwable $e) { echo get_class($e), ": ", $e->getMessage(), " prev=", $e->getPrevious() ? $e->getPrevious()->getMessage() : '-', "\n"; }
  var_dump($o->x);
  $r = &$o->x;
  try { $r += []; } catch (Throwable $e) { echo get_class($e), ": ", $e->getMessage(), " prev=", $e->getPrevious() ? $e->getPrevious()->getMessage() : '-', "\n"; }
  var_dump($o->x);
  try { C::$s += []; } catch (Throwable $e) { echo get_class($e), ": ", $e->getMessage(), "\n"; }
  var_dump(C::$s);
  $a = ['k' => &$o->x];
  try { $a['k'] += []; } catch (Throwable $e) { echo get_class($e), ": ", $e->getMessage(), "\n"; }
  var_dump($o->x);
  try { $o->x %= 0; } catch (Throwable $e) { echo get_class($e), ": ", $e->getMessage(), "\n"; }
  var_dump($o->x);
}
f(); f();
--EXPECT--
TypeError: Unsupported operand types: int + array prev=-
int(0)
TypeError: Unsupported operand types: int + array prev=-
int(0)
TypeError: Unsupported operand types: int + array
int(0)
TypeError: Unsupported operand types: int + array
int(0)
DivisionByZeroError: Modulo by zero
int(0)
TypeError: Unsupported operand types: int + array prev=-
int(0)
TypeError: Unsupported operand types: int + array prev=-
int(0)
TypeError: Unsupported operand types: int + array
int(0)
TypeError: Unsupported operand types: int + array
int(0)
DivisionByZeroError: Modulo by zero
int(0)
