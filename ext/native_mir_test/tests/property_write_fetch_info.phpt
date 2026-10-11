--TEST--
Native write fetches of properties take the typed property info from the run-time cache
--DESCRIPTION--
FETCH_OBJ_W/RW/FUNC_ARG/UNSET with fetch flags and ASSIGN_OBJ_REF use the info get_property_ptr_ptr caches, as zend_fetch_property_address does: a dynamic property named like a static property stays untyped, typed properties and typed references keep their checks.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
#[AllowDynamicProperties]
class S {
    public static int $name = 1;
    public static array $list = [];
}
class T {
    public int $n = 1;
    public ?int $nn = null;
    public array $items = [];
    public ?array $maybe = null;
    public int $notarr = 0;
    public $plain = null;
    public int $uninit;
    public readonly array $ro;
    public function __construct() { $this->ro = [1]; }
}
class Magic {
    private $d = [];
    public function &__get($n) { echo "get $n|"; if (!isset($this->d[$n])) $this->d[$n] = null; return $this->d[$n]; }
}
function grab(&$r) { return $r; }
function run($i) {
    $s = new S;
    $r = &$s->name;
    $r = "str$i";
    echo json_encode($s->name), "\n";
    $s->list[] = $i;
    echo json_encode($s->list), "\n";
    $prop = 'name';
    $r2 = &$s->$prop;
    $r2 = [$i];
    echo json_encode($s->name), "\n";
    $s->other = 1;
    $o = &$s->other;
    $o = 'x';
    $t = new T;
    $ref = &$t->n;
    try { $ref = 'nope'; } catch (TypeError $e) { echo $e->getMessage(), "\n"; }
    $ref = '5';
    var_dump($t->n);
    $t->items['k'] = $i;
    $t->items[] = 2;
    $t->maybe[] = 3;
    try { $t->notarr[] = 1; } catch (Error $e) { echo $e->getMessage(), "\n"; }
    $t->plain[] = 4;
    $name = 'items';
    $t->$name['d'] = 5;
    $name2 = 'notarr';
    try { $t->$name2[] = 1; } catch (Error $e) { echo $e->getMessage(), "\n"; }
    try { $u = &$t->uninit; } catch (Error $e) { echo $e->getMessage(), "\n"; }
    $nn = &$t->nn;
    $nn = 7;
    try { $nn = 'q'; } catch (TypeError $e) { echo $e->getMessage(), "\n"; }
    try { $t->ro[] = 2; } catch (Error $e) { echo $e->getMessage(), "\n"; }
    try { $rr = &$t->ro; } catch (Error $e) { echo $e->getMessage(), "\n"; }
    grab($t->n);
    grab($t->$name);
    $t->n = 3;
    $x = 9;
    $t->n = &$x;
    try { $x = 'bad'; } catch (TypeError $e) { echo $e->getMessage(), "\n"; }
    $s->name = &$x;
    try { $x = 'free'; } catch (TypeError $e) { echo $e->getMessage(), "\n"; }
    echo json_encode([$s->name, $t->n]), "\n";
    $y = 'notint';
    try { $t->n = &$y; } catch (TypeError $e) { echo $e->getMessage(), "\n"; }
    $pn = 'n';
    try { $t->$pn = &$y; } catch (TypeError $e) { echo $e->getMessage(), "\n"; }
    $m = new Magic;
    $m->a['x'] = 1;
    $mr = &$m->b;
    $mr = 2;
    unset($t->items['k']);
    unset($t->$name['d']);
    echo json_encode([$t->items, $t->maybe, $t->plain, $t->nn]), "\n";
    $objs = [new T, new S, new T];
    foreach ($objs as $obj) {
        try { $obj->n[] = 1; } catch (Error $e) { echo get_class($obj), ': ', $e->getMessage(), "\n"; }
        $q = &$obj->nn;
        try { $q = 'x'; } catch (TypeError $e) { echo get_class($obj), ': ', $e->getMessage(), "\n"; }
    }
}
run(1);
run(2);
--EXPECTF--

Notice: Accessing static property S::$name as non static in %s on line 25

Notice: Accessing static property S::$name as non static in %s on line 27
"str1"

Notice: Accessing static property S::$list as non static in %s on line 28

Notice: Accessing static property S::$list as non static in %s on line 29
[1]

Notice: Accessing static property S::$name as non static in %s on line 31

Notice: Accessing static property S::$name as non static in %s on line 33
[1]
Cannot assign string to reference held by property T::$n of type int
int(5)
Cannot use a scalar value as an array
Cannot use a scalar value as an array
Cannot access uninitialized non-nullable property T::$uninit by reference
Cannot assign string to reference held by property T::$nn of type ?int
Cannot indirectly modify readonly property T::$ro
Cannot indirectly modify readonly property T::$ro
Cannot assign string to reference held by property T::$n of type int

Notice: Accessing static property S::$name as non static in %s on line 63
Cannot assign string to reference held by property T::$n of type int

Notice: Accessing static property S::$name as non static in %s on line 65
[9,9]
Cannot assign string to property T::$n of type int
Cannot assign string to property T::$n of type int
get a|get b|[[2],[3],[4],7]
T: Cannot use a scalar value as an array
T: Cannot assign string to reference held by property T::$nn of type ?int
T: Cannot use a scalar value as an array
T: Cannot assign string to reference held by property T::$nn of type ?int

Notice: Accessing static property S::$name as non static in %s on line 25

Notice: Accessing static property S::$name as non static in %s on line 27
"str2"

Notice: Accessing static property S::$list as non static in %s on line 28

Notice: Accessing static property S::$list as non static in %s on line 29
[2]

Notice: Accessing static property S::$name as non static in %s on line 31

Notice: Accessing static property S::$name as non static in %s on line 33
[2]
Cannot assign string to reference held by property T::$n of type int
int(5)
Cannot use a scalar value as an array
Cannot use a scalar value as an array
Cannot access uninitialized non-nullable property T::$uninit by reference
Cannot assign string to reference held by property T::$nn of type ?int
Cannot indirectly modify readonly property T::$ro
Cannot indirectly modify readonly property T::$ro
Cannot assign string to reference held by property T::$n of type int

Notice: Accessing static property S::$name as non static in %s on line 63
Cannot assign string to reference held by property T::$n of type int

Notice: Accessing static property S::$name as non static in %s on line 65
[9,9]
Cannot assign string to property T::$n of type int
Cannot assign string to property T::$n of type int
get a|get b|[[2],[3],[4],7]
T: Cannot use a scalar value as an array
T: Cannot assign string to reference held by property T::$nn of type ?int
T: Cannot use a scalar value as an array
T: Cannot assign string to reference held by property T::$nn of type ?int
