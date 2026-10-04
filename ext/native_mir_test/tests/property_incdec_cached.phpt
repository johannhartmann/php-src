--TEST--
Property ++/-- through the run-time cache and FETCH_THIS without decoding
--DESCRIPTION--
++ and -- of an untyped declared integer property of $this or a CV take the
cached fast path, with the result in a temporary or a CV; typed, readonly,
unset, magic, overflowing and non-integer properties keep the general
path. $this sent to a function is fetched without decoding.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class Counter {
    public $n = 0; private $level = 0; public int $typed = 5; public $f = 1.5;
    public function __construct(public readonly int $ro = 1) {}
    public function run() {
        $a = $this->n++; $b = ++$this->n; ++$this->level; $depth = $this->level--;
        $s = 'old' . $a; $s = $this->n--; $this->typed++; $this->f++;
        return [$a, $b, $depth, $s, $this->n, $this->level, $this->typed, $this->f, self::who($this)];
    }
    static function who($o) { return get_class($o); }
}
class Magic { private $data = ['m' => 1]; function __get($k) { echo "get "; return $this->data[$k]; } function __set($k, $v) { echo "set "; $this->data[$k] = $v; } }
$c = new Counter;
for ($i = 0; $i < 3; $i++) { echo json_encode($c->run()), "\n"; }
$o = new Counter; $o->n = PHP_INT_MAX; $o->n++; var_dump($o->n);
$o->n = 'a9'; $o->n++; var_dump($o->n);
unset($o->n); $m = new Magic; $m->m++; echo "|\n";
try { $o->ro++; } catch (Error $e) { echo get_class($e), ": ", $e->getMessage(), "\n"; }
try { $o->typed = PHP_INT_MAX; $o->typed++; } catch (Error $e) { echo get_class($e), "\n"; }
?>
--EXPECTF--
[0,2,1,2,1,0,6,2.5,"Counter"]
[1,3,1,3,2,0,7,3.5,"Counter"]
[2,4,1,4,3,0,8,4.5,"Counter"]
float(9.223372036854776E+18)

Deprecated: Increment on non-numeric string is deprecated, use str_increment() instead in %s on line 16
string(2) "b0"
get set |
Error: Cannot modify readonly property Counter::$ro
TypeError
