--TEST--
Native FETCH_CLASS_CONSTANT through the run-time cache
--DESCRIPTION--
Class constants of a literal name are read through the run-time cache the
VM fills: literal classes, self::, parent:: and static:: with overriding
constants, constant expressions, deprecated constants, visibility errors,
enum cases and interface constants keep stock semantics.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
interface HasSize { const SIZE = 4; }
enum Suit: string { case Hearts = 'H'; case Spades = 'S'; const Wild = self::Spades; }
class CBase implements HasSize {
    const NAME = 'base';
    const LIST = [1, 2, 3];
    const EXPR = self::NAME . '-expr';
    const LATE = PHP_INT_SIZE * 2;
    private const SECRET = 's';
    #[\Deprecated] const OLD = 'old';
    static function names() { return [self::NAME, static::NAME, self::EXPR, static::SIZE, self::LIST[1], self::LATE]; }
    static function secret() { return self::SECRET; }
    static function old() { return self::OLD; }
}
class CChild extends CBase {
    const NAME = 'child';
    static function parents() { return [parent::NAME, self::NAME, static::NAME, parent::EXPR]; }
}
function read($r) {
    return [CBase::NAME, CChild::NAME, CBase::names(), CChild::names(), CChild::parents(),
        HasSize::SIZE, CChild::SIZE, Suit::Hearts->value, Suit::Wild->name, CBase::LIST, $r];
}
for ($i = 0; $i < 3; $i++) {
    echo json_encode(read($i)), "\n";
    echo CBase::secret(), "|", CBase::old(), "\n";
    try { echo CBase::SECRET; } catch (Error $e) { echo get_class($e), ": ", $e->getMessage(), "\n"; }
}
?>
--EXPECTF--
["base","child",["base","base","base-expr",4,2,16],["base","child","base-expr",4,2,16],["base","child","child","base-expr"],4,4,"H","Spades",[1,2,3],0]
s|
Deprecated: Constant CBase::OLD is deprecated in %s on line %d
old
Error: Cannot access private constant CBase::SECRET
["base","child",["base","base","base-expr",4,2,16],["base","child","base-expr",4,2,16],["base","child","child","base-expr"],4,4,"H","Spades",[1,2,3],1]
s|
Deprecated: Constant CBase::OLD is deprecated in %s on line %d
old
Error: Cannot access private constant CBase::SECRET
["base","child",["base","base","base-expr",4,2,16],["base","child","base-expr",4,2,16],["base","child","child","base-expr"],4,4,"H","Spades",[1,2,3],2]
s|
Deprecated: Constant CBase::OLD is deprecated in %s on line %d
old
Error: Cannot access private constant CBase::SECRET
