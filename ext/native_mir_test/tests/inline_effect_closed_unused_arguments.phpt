--TEST--
Native effect-closed inline bodies skip the arguments they never read
--DESCRIPTION--
An inlined effect-closed callee neither transports nor unboxes an argument
its body never reads; an unread argument whose declared type the receive
checks still has to be proven by the caller. Results and by-reference
arguments match the VM.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function second(int $a, int $b): int { return $b + 1; }
function first_only(int $a, bool $flag, int $c): int { return $a * 2; }
function pick(bool $c, int $a, int $b): int { return $c ? $a : 7; }
function ignore_all(int $a, int $b): int { return 42; }
function ignore_ref(int &$r, int $b): int { return $b - 1; }
function ignore_string(string $s, int $b): int { return $b; }
function ignore_untyped($u, int $b): int { return $b * 3; }
function caller(int $n) {
    $acc = 0;
    $t = true;
    $ref = 3;
    for ($i = 0; $i < $n; $i++) {
        $acc += second($i, $i + 1);
        $acc += first_only($i, $t, PHP_INT_MAX);
        $acc += pick($i % 2 == 0, $i, $acc);
        $acc += ignore_all($acc, $i);
        $acc += ignore_ref($ref, $i);
        $acc += ignore_string("x$i", $i);
        $acc += ignore_untyped([$i], $i);
        $acc += ignore_untyped(new stdClass, $i);
        $t = !$t;
    }
    return [$acc, $ref];
}
function untyped_caller($n, $v) {
    $acc = 0;
    for ($i = 0; $i < $n; $i++) {
        $acc += second($i, $v);
        $acc += ignore_untyped($v, $i);
    }
    return $acc;
}
for ($r = 0; $r < 3; $r++) {
    var_dump(caller(5), caller(0));
    var_dump(untyped_caller(3, 4), untyped_caller(2, -1));
}
?>
--EXPECT--
array(2) {
  [0]=>
  int(345)
  [1]=>
  int(3)
}
array(2) {
  [0]=>
  int(0)
  [1]=>
  int(3)
}
int(24)
int(3)
array(2) {
  [0]=>
  int(345)
  [1]=>
  int(3)
}
array(2) {
  [0]=>
  int(0)
  [1]=>
  int(3)
}
int(24)
int(3)
array(2) {
  [0]=>
  int(345)
  [1]=>
  int(3)
}
array(2) {
  [0]=>
  int(0)
  [1]=>
  int(3)
}
int(24)
int(3)
