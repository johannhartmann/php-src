--TEST--
Native: A union of a scalar and a class type is not an exact scalar type
--DESCRIPTION--
ZEND_TYPE_PURE_MASK() of int|Foo shows only MAY_BE_LONG; the class name
admits objects, so neither the received argument nor the declared return
value has an exact integer type.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
namespace NativeUnion;
enum Level: int { case Info = 200; }
class Foo {}
class Logger {
    public function add(int|Level $level, string $message): string {
        if (is_int($level)) {
            return "int $level $message";
        }
        return "enum {$level->value} $message";
    }
    public function make(bool $object): int|Foo { return $object ? new Foo : 7; }
}
$l = new Logger;
var_dump($l->add(200, 'a'), $l->add(Level::Info, 'b'));
var_dump($l->make(false), get_class($l->make(true)));
--EXPECT--
string(9) "int 200 a"
string(10) "enum 200 b"
int(7)
string(15) "NativeUnion\Foo"
