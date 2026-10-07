--TEST--
Fast-call entries release $this on leave: kept, temporary and destroyed receivers, throwing destructors, discarded results
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
$dir = sys_get_temp_dir() . '/fast_leave_this_' . getmypid();
@mkdir($dir);
$lib = $dir . '/lib.php';
file_put_contents($lib, <<<'LIB'
<?php
class P { public $n = 0; public $log; function __construct($l = null) { $this->log = $l; } function inc($x) { $this->n += $x; return $this; } function name() { return 'p' . $this->n; }
  function __destruct() { if ($this->log) echo "destruct {$this->log}\n"; } }
class T { function __destruct() { throw new Exception('in destructor'); } function m() { return 5; } }
function make($l) { return new P($l); }
LIB);
require $lib;
$p = new P();
$t = 0;
for ($i = 0; $i < 1000; $i++) { $t += $p->inc(1)->n; }
echo $t, ' ', $p->name(), "\n";
for ($i = 0; $i < 3; $i++) { echo make("tmp$i")->inc(2)->name(), "\n"; }
make('x')->inc(1);
echo "after discard\n";
try { $v = (new T)->m(); echo "v=$v\n"; } catch (Exception $e) { echo 'caught ', $e->getMessage(), "\n"; }
try { (new T)->m(); } catch (Exception $e) { echo 'caught2 ', $e->getMessage(), "\n"; }
$objs = [];
for ($i = 0; $i < 3000; $i++) { $o = new P(); $o->inc($i); $objs[] = $o->name(); }
echo count($objs), ' ', $objs[2999], "\n";
gc_collect_cycles();
echo "end\n";
unlink($lib); rmdir($dir);
--EXPECT--
500500 p1000
destruct tmp0
p2
destruct tmp1
p2
destruct tmp2
p2
destruct x
after discard
caught in destructor
caught2 in destructor
3000 p2999
end
