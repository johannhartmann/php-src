--TEST--
Native static property accesses read the run-time cache like the VM
--DESCRIPTION--
Literal, self:: and parent:: static property reads, writes, increments,
compound assignments and isset checks hit the cached property after the
first access; a compound assignment's cache slot is the OP_DATA's, so it
must not overwrite neighbouring cache slots. A typed property read before
initialization still throws on the cached path.
--EXTENSIONS--
native_mir_test
--FILE--
<?php
class Base {
    public static $count = 0;
    public static string $text = '';
    public static int $typed;
    protected static $items = [];
}
class Child extends Base {
    public static function add($item) {
        self::$items[] = $item;
        parent::$count++;
        static::$text .= $item;
        Base::$count += 10;
        return count(self::$items);
    }
    public static function items() {
        return parent::$items;
    }
}
function run($n) {
    $out = [];
    for ($i = 0; $i < $n; $i++) {
        $out[] = Child::add("x$i");
        Base::$text .= '-';
        $out[] = isset(Base::$count) ? Base::$count : -1;
        $out[] = empty(Base::$text) ? 'empty' : Base::$text;
        try {
            $out[] = Base::$typed;
        } catch (Error $e) {
            $out[] = $e->getMessage();
        }
    }
    Base::$typed = 7;
    $out[] = Base::$typed;
    $out[] = implode(',', Child::items());
    return $out;
}
var_dump(run(3));
var_dump(Base::$count, Base::$text);
?>
--EXPECT--
array(14) {
  [0]=>
  int(1)
  [1]=>
  int(11)
  [2]=>
  string(3) "x0-"
  [3]=>
  string(77) "Typed static property Base::$typed must not be accessed before initialization"
  [4]=>
  int(2)
  [5]=>
  int(22)
  [6]=>
  string(6) "x0-x1-"
  [7]=>
  string(77) "Typed static property Base::$typed must not be accessed before initialization"
  [8]=>
  int(3)
  [9]=>
  int(33)
  [10]=>
  string(9) "x0-x1-x2-"
  [11]=>
  string(77) "Typed static property Base::$typed must not be accessed before initialization"
  [12]=>
  int(7)
  [13]=>
  string(8) "x0,x1,x2"
}
int(33)
string(9) "x0-x1-x2-"
