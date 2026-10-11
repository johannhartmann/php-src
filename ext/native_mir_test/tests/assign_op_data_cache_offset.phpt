--TEST--
Native compound property assignment takes the OP_DATA cache offset from the compiler
--DESCRIPTION--
ASSIGN_OBJ_OP and ASSIGN_STATIC_PROP_OP helpers receive the OP_DATA's
run-time cache offset in the auxiliary operand: cached integer updates,
string appends, typed, magic, dynamic and static (self, static, parent,
named) properties, warmed over several iterations and classes.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class Base {
    public static $s = 1;
    public static string $str = 'a';
}
class Box extends Base {
    public $a = 0;
    public $b = 'x';
    public int $typed = 5;
    public static $s = 10;
    public static $t = 100;
    private array $magic = [];
    public function __get($n) { echo "get $n\n"; return $this->magic[$n] ?? 1; }
    public function __set($n, $v) { echo "set $n\n"; $this->magic[$n] = $v; }
    function run($n) {
        for ($i = 0; $i < $n; $i++) {
            $this->a += 2;
            $this->a -= 1;
            $this->b .= 'y';
            $this->typed *= 2;
            self::$s += 1;
            static::$t -= 2;
            parent::$s .= '';
            Base::$str .= 'b';
            self::$t <<= 1;
        }
        return [$this->a, $this->b, $this->typed, self::$s, static::$t, parent::$s, Base::$str];
    }
}
class Sub extends Box { public static $t = 7; }
function other(Box $o, $name) {
    $o->a += 5;
    $o->$name .= 'z';
    $o->dyn += 3;
    try { $o->typed .= 'q'; } catch (TypeError $e) { echo get_class($e), ": ", $e->getMessage(), "\n"; }
    $o->a += PHP_INT_MAX;
    return $o;
}
$o = new Box;
var_dump($o->run(3));
$p = new Sub;
var_dump($p->run(2));
var_dump(other($o, 'b'));
var_dump(other(new Sub, 'b'));
try { $n = null; $n->x += 1; } catch (Error $e) { echo $e->getMessage(), "\n"; }
try { $undef->x .= 1; } catch (Error $e) { echo $e->getMessage(), "\n"; }
?>
--EXPECTF--
array(7) {
  [0]=>
  int(3)
  [1]=>
  string(4) "xyyy"
  [2]=>
  int(40)
  [3]=>
  int(13)
  [4]=>
  int(772)
  [5]=>
  string(1) "1"
  [6]=>
  string(4) "abbb"
}
array(7) {
  [0]=>
  int(2)
  [1]=>
  string(3) "xyy"
  [2]=>
  int(20)
  [3]=>
  int(15)
  [4]=>
  int(3)
  [5]=>
  string(1) "1"
  [6]=>
  string(6) "abbbbb"
}
get dyn
set dyn
TypeError: Cannot assign string to property Box::$typed of type int
object(Box)#1 (4) {
  ["a"]=>
  float(9.223372036854776E+18)
  ["b"]=>
  string(5) "xyyyz"
  ["typed"]=>
  int(40)
  ["magic":"Box":private]=>
  array(1) {
    ["dyn"]=>
    int(4)
  }
}
get dyn
set dyn
TypeError: Cannot assign string to property Box::$typed of type int
object(Sub)#3 (4) {
  ["a"]=>
  float(9.223372036854776E+18)
  ["b"]=>
  string(2) "xz"
  ["typed"]=>
  int(5)
  ["magic":"Box":private]=>
  array(1) {
    ["dyn"]=>
    int(4)
  }
}
Attempt to assign property "x" on null

Warning: Undefined variable $undef in %s on line 46
Attempt to assign property "x" on null
