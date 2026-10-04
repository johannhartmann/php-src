--TEST--
Compound property assignment through the OP_DATA run-time cache slot
--DESCRIPTION--
$receiver->name += and -= of an integer to an untyped declared integer
property of $this or a CV, without a used result, take the cached fast
path; overflow, other types and operators, used results, typed and magic
properties keep the general path.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class Parser { private $at = 0; public $hits = 0; public int $typed = 1; public $s = 'x'; public $f = 0.5;
    function step($n) { $this->at += $n; $this->at -= 1; $this->hits += 1; $this->typed += $n; $this->s .= 'y'; $this->f += $n; $r = ($this->hits += 2); return [$this->at, $this->hits, $this->typed, $this->s, $this->f, $r]; } }
class Bag { private $d = ['v' => 1]; function __get($k) { return $this->d[$k]; } function __set($k, $v) { echo "set$v "; $this->d[$k] = $v; } }
$p = new Parser;
for ($i = 0; $i < 3; $i++) { echo json_encode($p->step($i + 2)), "\n"; }
$q = new Parser; $q->hits = PHP_INT_MAX; $q->hits += 1; var_dump($q->hits);
$q->hits = 5; $k = 3; $q->hits += $k; $q->hits -= '2'; var_dump($q->hits);
$b = new Bag; $b->v += 4; echo "|\n";
try { $q->typed = PHP_INT_MAX; $q->typed += 1; } catch (Error $e) { echo get_class($e), "\n"; }
?>
--EXPECT--
[1,3,3,"xy",2.5,3]
[3,6,6,"xyy",5.5,6]
[6,9,10,"xyyy",9.5,9]
float(9.223372036854776E+18)
int(6)
set5 |
TypeError
