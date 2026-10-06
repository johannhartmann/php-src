--TEST--
Assignments under string keys: insertion, replacement, nested, properties
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class Box { public array $arr = []; public $loose; public ?array $typed = null; }
class Noisy { public $n; function __construct($n) { $this->n = $n; } function __destruct() { echo "destruct {$this->n}\n"; } }
function literal_keys() {
    $a = [];
    for ($i = 0; $i < 3; $i++) {
        $a['name'] = "v$i";
        $a['list'] = [$i, $i + 1];
        $a['obj'] = new Noisy($i);
        $a['x'] = $i;
    }
    return $a;
}
function runtime_keys(array $keys, $value) {
    $a = [];
    foreach ($keys as $k) {
        $a[$k] = $value;
    }
    foreach ($keys as $k) {
        $a[$k] = [$k];
    }
    return $a;
}
function dynamic_keys($n) {
    $a = ['pre' => 1];
    for ($i = 0; $i < $n; $i++) {
        $k = 'key' . $i;          // a fresh string without its hash
        $a[$k] = $i;
        $k2 = str_repeat('y', $i + 1) . 'long-key-beyond-sixteen-bytes';
        $a[$k2] = [$i];
        $a[$k2] = $i;
    }
    return $a;
}
function numeric_strings() {
    $a = [];
    foreach (['1', '-2', '01', '1.5', '', ' 3', '9223372036854775808', '-0', 'a1'] as $k) {
        $a[$k] = $k;
    }
    return $a;
}
function packed_to_hash() {
    $a = [1, 2, 3];
    $k = 'str';
    $a[$k] = 4;
    $a['lit'] = 5;
    $a[] = 6;
    return $a;
}
function nested(array $blocks, array $states, $styles) {
    $schema = [];
    foreach ($blocks as $b) {
        $schema[$b] = $styles;
        $schema[$b]['elements'] = $styles;
        foreach ($states as $s) {
            $schema[$b][$s] = $styles;
            $schema[$b][$s]['elements'] = $styles;
        }
    }
    return $schema;
}
function shared_container() {
    $a = ['k' => 1];
    $b = $a;
    $a['k'] = 2;
    $a['n'] = 3;
    $k = 'm';
    $c = $a;
    $a[$k] = 4;
    return [$a, $b, $c];
}
function referenced() {
    $a = ['k' => 1];
    $r = &$a;
    $a['k'] = 2;
    $a['z'] = 3;
    $v = 5;
    $ref = &$v;
    $a['ref'] = $v;
    $a['e'] = 1;
    $x = &$a['e'];
    $a['e'] = 9;
    return [$a, $r, $x];
}
function properties() {
    $o = new Box;
    $names = ['a', 'b', 'c'];
    foreach ($names as $n) {
        $o->arr[$n] = $n . '!';
        $o->arr[$n] = [$n];
        $o->loose[$n] = 1;
        $o->arr['lit'] = $n;
    }
    try { $o->typed['x'] = 1; } catch (Error $e) { echo get_class($e), ': ', $e->getMessage(), "\n"; }
    return $o;
}
function self_assign() {
    $a = ['x' => 1];
    $a['self'] = $a;
    $n = 'a';
    $b = ['y' => 2];
    $k = 'copy';
    $b[$k] = $b;
    return [$a, $b];
}
function replaced_objects() {
    $a = [];
    for ($i = 0; $i < 3; $i++) {
        $a['o'] = new Noisy("r$i");
    }
    echo "after loop\n";
    $a['o'] = null;
    echo "end\n";
}
function undefined_value() {
    $a = [];
    $a['u'] = @$undef;
    $a['n'] = null;
    return $a;
}
function non_array_containers() {
    $s = null;
    $s['k'] = 1;
    $u['k'] = 2;
    $f = false;
    try { $f['k'] = 3; } catch (Error $e) { echo $e->getMessage(), "\n"; }
    $str = 'abc';
    try { $str['k'] = 'x'; } catch (Error $e) { echo get_class($e), ': ', $e->getMessage(), "\n"; }
    $i = 5;
    try { $i['k'] = 1; } catch (Error $e) { echo $e->getMessage(), "\n"; }
    $obj = new ArrayObject();
    $obj['k'] = 4;
    return [$s, $u, $f, $str, $i, $obj->getArrayCopy()];
}
function temporary_keys($p) {
    $a = [];
    $a[$p . 'x'] = 1;
    $a[strtoupper($p)] = 2;
    $a[(string) 5] = 3;
    return $a;
}
function globals_assign() {
    global $g;
    $g['k'] = 1;
    $g['k2'] = [1];
    $GLOBALS['h']['k'] = 2;
}
var_dump(literal_keys());
var_dump(runtime_keys(['a', 'bb', 'a', 'ccc'], 'val'));
var_dump(dynamic_keys(3));
var_dump(numeric_strings());
var_dump(packed_to_hash());
var_dump(nested(['core/a', 'core/b'], ['sm', 'md'], ['color' => 1]));
var_dump(shared_container());
var_dump(referenced());
var_dump(properties());
var_dump(self_assign());
replaced_objects();
var_dump(undefined_value());
var_dump(non_array_containers());
var_dump(temporary_keys('p'));
$g = [];
globals_assign();
var_dump($g, $h);
$main = [];
for ($i = 0; $i < 2; $i++) { $main['k' . $i] = $i; $main['lit'] = [$i]; }
var_dump($main);
echo "done\n";
--EXPECTF--
destruct 0
destruct 1
array(4) {
  ["name"]=>
  string(2) "v2"
  ["list"]=>
  array(2) {
    [0]=>
    int(2)
    [1]=>
    int(3)
  }
  ["obj"]=>
  object(Noisy)#1 (1) {
    ["n"]=>
    int(2)
  }
  ["x"]=>
  int(2)
}
destruct 2
array(3) {
  ["a"]=>
  array(1) {
    [0]=>
    string(1) "a"
  }
  ["bb"]=>
  array(1) {
    [0]=>
    string(2) "bb"
  }
  ["ccc"]=>
  array(1) {
    [0]=>
    string(3) "ccc"
  }
}
array(7) {
  ["pre"]=>
  int(1)
  ["key0"]=>
  int(0)
  ["ylong-key-beyond-sixteen-bytes"]=>
  int(0)
  ["key1"]=>
  int(1)
  ["yylong-key-beyond-sixteen-bytes"]=>
  int(1)
  ["key2"]=>
  int(2)
  ["yyylong-key-beyond-sixteen-bytes"]=>
  int(2)
}
array(9) {
  [1]=>
  string(1) "1"
  [-2]=>
  string(2) "-2"
  ["01"]=>
  string(2) "01"
  ["1.5"]=>
  string(3) "1.5"
  [""]=>
  string(0) ""
  [" 3"]=>
  string(2) " 3"
  ["9223372036854775808"]=>
  string(19) "9223372036854775808"
  ["-0"]=>
  string(2) "-0"
  ["a1"]=>
  string(2) "a1"
}
array(6) {
  [0]=>
  int(1)
  [1]=>
  int(2)
  [2]=>
  int(3)
  ["str"]=>
  int(4)
  ["lit"]=>
  int(5)
  [3]=>
  int(6)
}
array(2) {
  ["core/a"]=>
  array(4) {
    ["color"]=>
    int(1)
    ["elements"]=>
    array(1) {
      ["color"]=>
      int(1)
    }
    ["sm"]=>
    array(2) {
      ["color"]=>
      int(1)
      ["elements"]=>
      array(1) {
        ["color"]=>
        int(1)
      }
    }
    ["md"]=>
    array(2) {
      ["color"]=>
      int(1)
      ["elements"]=>
      array(1) {
        ["color"]=>
        int(1)
      }
    }
  }
  ["core/b"]=>
  array(4) {
    ["color"]=>
    int(1)
    ["elements"]=>
    array(1) {
      ["color"]=>
      int(1)
    }
    ["sm"]=>
    array(2) {
      ["color"]=>
      int(1)
      ["elements"]=>
      array(1) {
        ["color"]=>
        int(1)
      }
    }
    ["md"]=>
    array(2) {
      ["color"]=>
      int(1)
      ["elements"]=>
      array(1) {
        ["color"]=>
        int(1)
      }
    }
  }
}
array(3) {
  [0]=>
  array(3) {
    ["k"]=>
    int(2)
    ["n"]=>
    int(3)
    ["m"]=>
    int(4)
  }
  [1]=>
  array(1) {
    ["k"]=>
    int(1)
  }
  [2]=>
  array(2) {
    ["k"]=>
    int(2)
    ["n"]=>
    int(3)
  }
}
array(3) {
  [0]=>
  array(4) {
    ["k"]=>
    int(2)
    ["z"]=>
    int(3)
    ["ref"]=>
    int(5)
    ["e"]=>
    int(9)
  }
  [1]=>
  array(4) {
    ["k"]=>
    int(2)
    ["z"]=>
    int(3)
    ["ref"]=>
    int(5)
    ["e"]=>
    int(9)
  }
  [2]=>
  int(9)
}
object(Box)#1 (3) {
  ["arr"]=>
  array(4) {
    ["a"]=>
    array(1) {
      [0]=>
      string(1) "a"
    }
    ["lit"]=>
    string(1) "c"
    ["b"]=>
    array(1) {
      [0]=>
      string(1) "b"
    }
    ["c"]=>
    array(1) {
      [0]=>
      string(1) "c"
    }
  }
  ["loose"]=>
  array(3) {
    ["a"]=>
    int(1)
    ["b"]=>
    int(1)
    ["c"]=>
    int(1)
  }
  ["typed"]=>
  array(1) {
    ["x"]=>
    int(1)
  }
}
array(2) {
  [0]=>
  array(2) {
    ["x"]=>
    int(1)
    ["self"]=>
    array(1) {
      ["x"]=>
      int(1)
    }
  }
  [1]=>
  array(2) {
    ["y"]=>
    int(2)
    ["copy"]=>
    array(1) {
      ["y"]=>
      int(2)
    }
  }
}
destruct r0
destruct r1
after loop
end
destruct r2
array(2) {
  ["u"]=>
  NULL
  ["n"]=>
  NULL
}

Deprecated: Automatic conversion of false to array is deprecated in %s on line %d
TypeError: Cannot access offset of type string on string
Cannot use a scalar value as an array
array(6) {
  [0]=>
  array(1) {
    ["k"]=>
    int(1)
  }
  [1]=>
  array(1) {
    ["k"]=>
    int(2)
  }
  [2]=>
  array(1) {
    ["k"]=>
    int(3)
  }
  [3]=>
  string(3) "abc"
  [4]=>
  int(5)
  [5]=>
  array(1) {
    ["k"]=>
    int(4)
  }
}
array(3) {
  ["px"]=>
  int(1)
  ["P"]=>
  int(2)
  [5]=>
  int(3)
}
array(2) {
  ["k"]=>
  int(1)
  ["k2"]=>
  array(1) {
    [0]=>
    int(1)
  }
}
array(1) {
  ["k"]=>
  int(2)
}
array(3) {
  ["k0"]=>
  int(0)
  ["lit"]=>
  array(1) {
    [0]=>
    int(1)
  }
  ["k1"]=>
  int(1)
}
done
