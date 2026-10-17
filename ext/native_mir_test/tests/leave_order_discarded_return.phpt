--TEST--
A leaving frame is not current while its values die, in the VM's order
--DESCRIPTION--
As zend_leave_helper(), the caller is current before a frame's CVs are
released. A discarded return value dies as the VM's RETURN without a return
value releases it: a temporary at the return, while the callee is current;
a returned CV in CV order with the other CVs.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
final class Probe
{
    public function __construct(public string $name) {}
    public function __destruct()
    {
        $frames = array_map(fn($f) => ($f['class'] ?? '') . ($f['type'] ?? '') . $f['function'], debug_backtrace());
        echo "destruct {$this->name}: ", implode(' <- ', $frames), "\n";
    }
}

function local_object(int $n) { $p = new Probe("local$n"); return $n + 1; }
function returns_first(int $n) { $a = new Probe("a$n"); $b = new Probe("b$n"); return $a; }
function returns_last(int $n) { $a = new Probe("a$n"); $b = new Probe("b$n"); return $b; }
function returns_tmp(int $n) { $b = new Probe("local$n"); return new Probe("tmp$n"); }
function returns_reference(int $n) { $a = new Probe("r$n"); $r = &$a; $b = new Probe("s$n"); return $r; }

final class Service
{
    public function first(int $n) { $a = new Probe("ma$n"); $b = new Probe("mb$n"); return $a; }
    public function last(int $n) { $a = new Probe("ma$n"); $b = new Probe("mb$n"); return $b; }
    public static function stat(int $n) { $a = new Probe("sa$n"); $b = new Probe("sb$n"); return $a; }
    public function __call($m, $args) { $p = new Probe("magic$m"); return 1; }
    public function run(int $n)
    {
        $this->first($n);
        $this->last($n);
        self::stat($n);
        $this->missing($n);
        return 0;
    }
}

function discard_all(int $n)
{
    returns_first($n);
    returns_last($n);
    returns_tmp($n);
    returns_reference($n);
    call_user_func('returns_first', $n);
    $f = 'returns_last';
    $f($n);
    $c = function ($k) { $x = new Probe("cx$k"); $y = new Probe("cy$k"); return $x; };
    $c($n);
    (new Service())->run($n);
    return 1;
}

function used_results(int $n)
{
    $kept = returns_first($n);
    echo "kept {$kept->name}\n";
    var_dump(call_user_func('local_object', $n), array_map('local_object', [$n]));
    return 2;
}

for ($i = 0; $i < 3; $i++) {
    echo "-- $i\n";
    var_dump(discard_all($i), used_results($i));
}
--EXPECT--
-- 0
destruct a0: Probe->__destruct <- discard_all
destruct b0: Probe->__destruct <- discard_all
destruct a0: Probe->__destruct <- discard_all
destruct b0: Probe->__destruct <- discard_all
destruct tmp0: Probe->__destruct <- returns_tmp <- discard_all
destruct local0: Probe->__destruct <- discard_all
destruct r0: Probe->__destruct <- discard_all
destruct s0: Probe->__destruct <- discard_all
destruct a0: Probe->__destruct <- discard_all
destruct b0: Probe->__destruct <- discard_all
destruct a0: Probe->__destruct <- discard_all
destruct b0: Probe->__destruct <- discard_all
destruct cx0: Probe->__destruct <- discard_all
destruct cy0: Probe->__destruct <- discard_all
destruct ma0: Probe->__destruct <- Service->run <- discard_all
destruct mb0: Probe->__destruct <- Service->run <- discard_all
destruct ma0: Probe->__destruct <- Service->run <- discard_all
destruct mb0: Probe->__destruct <- Service->run <- discard_all
destruct sa0: Probe->__destruct <- Service->run <- discard_all
destruct sb0: Probe->__destruct <- Service->run <- discard_all
destruct magicmissing: Probe->__destruct <- Service->run <- discard_all
destruct b0: Probe->__destruct <- used_results
kept a0
destruct local0: Probe->__destruct <- used_results
destruct local0: Probe->__destruct <- array_map <- used_results
int(1)
array(1) {
  [0]=>
  int(1)
}
destruct a0: Probe->__destruct
int(1)
int(2)
-- 1
destruct a1: Probe->__destruct <- discard_all
destruct b1: Probe->__destruct <- discard_all
destruct a1: Probe->__destruct <- discard_all
destruct b1: Probe->__destruct <- discard_all
destruct tmp1: Probe->__destruct <- returns_tmp <- discard_all
destruct local1: Probe->__destruct <- discard_all
destruct r1: Probe->__destruct <- discard_all
destruct s1: Probe->__destruct <- discard_all
destruct a1: Probe->__destruct <- discard_all
destruct b1: Probe->__destruct <- discard_all
destruct a1: Probe->__destruct <- discard_all
destruct b1: Probe->__destruct <- discard_all
destruct cx1: Probe->__destruct <- discard_all
destruct cy1: Probe->__destruct <- discard_all
destruct ma1: Probe->__destruct <- Service->run <- discard_all
destruct mb1: Probe->__destruct <- Service->run <- discard_all
destruct ma1: Probe->__destruct <- Service->run <- discard_all
destruct mb1: Probe->__destruct <- Service->run <- discard_all
destruct sa1: Probe->__destruct <- Service->run <- discard_all
destruct sb1: Probe->__destruct <- Service->run <- discard_all
destruct magicmissing: Probe->__destruct <- Service->run <- discard_all
destruct b1: Probe->__destruct <- used_results
kept a1
destruct local1: Probe->__destruct <- used_results
destruct local1: Probe->__destruct <- array_map <- used_results
int(2)
array(1) {
  [0]=>
  int(2)
}
destruct a1: Probe->__destruct
int(1)
int(2)
-- 2
destruct a2: Probe->__destruct <- discard_all
destruct b2: Probe->__destruct <- discard_all
destruct a2: Probe->__destruct <- discard_all
destruct b2: Probe->__destruct <- discard_all
destruct tmp2: Probe->__destruct <- returns_tmp <- discard_all
destruct local2: Probe->__destruct <- discard_all
destruct r2: Probe->__destruct <- discard_all
destruct s2: Probe->__destruct <- discard_all
destruct a2: Probe->__destruct <- discard_all
destruct b2: Probe->__destruct <- discard_all
destruct a2: Probe->__destruct <- discard_all
destruct b2: Probe->__destruct <- discard_all
destruct cx2: Probe->__destruct <- discard_all
destruct cy2: Probe->__destruct <- discard_all
destruct ma2: Probe->__destruct <- Service->run <- discard_all
destruct mb2: Probe->__destruct <- Service->run <- discard_all
destruct ma2: Probe->__destruct <- Service->run <- discard_all
destruct mb2: Probe->__destruct <- Service->run <- discard_all
destruct sa2: Probe->__destruct <- Service->run <- discard_all
destruct sb2: Probe->__destruct <- Service->run <- discard_all
destruct magicmissing: Probe->__destruct <- Service->run <- discard_all
destruct b2: Probe->__destruct <- used_results
kept a2
destruct local2: Probe->__destruct <- used_results
destruct local2: Probe->__destruct <- array_map <- used_results
int(3)
array(1) {
  [0]=>
  int(3)
}
destruct a2: Probe->__destruct
int(1)
int(2)
