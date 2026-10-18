--TEST--
Native: a nullsafe chain that short-circuits yields its own result over a reused temporary
--DESCRIPTION--
The optimizer gives the short-circuit result of `?->` the temporary of an
earlier arithmetic result that the native code keeps in a register. The
short-circuit writes null (false for isset, true for empty) to the slot; the
result merge after the chain must read that slot, not the register's stale
value of the earlier result or of the previous iteration's chain.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class Y { public $p = "y"; public $q = null; }

function sub_keys(int $n) {
    $c = [new Y, null];
    foreach ([0, 1, $n - 1] as $v) {
        var_dump($c[$v]?->p);
    }
}

function spaceship_keys(int $n) {
    $c = [new Y, null];
    $l = [0, 1, $n <=> 1];
    foreach ($l as $v) {
        var_dump($c[$v]?->p);
    }
}

function loop_keys(int $n) {
    $c = [new Y, null];
    for ($i = 0; $i < 3; $i++) {
        $k = $n - 1 + ($i % 2);
        var_dump($c[$k]?->p);
    }
}

function cv_chain(int $n) {
    $c = [new Y, null];
    foreach ([0, 1, $n - 1] as $v) {
        $t = $c[$v];
        var_dump($t?->p);
    }
}

function isset_empty_chain(int $n) {
    $c = [new Y, null];
    foreach ([0, 1, $n - 1] as $v) {
        var_dump(isset($c[$v]?->p), empty($c[$v]?->p), $c[$v]?->q?->p);
    }
}

sub_keys(1);
spaceship_keys(1);
loop_keys(1);
cv_chain(1);
isset_empty_chain(1);
?>
--EXPECT--
string(1) "y"
NULL
string(1) "y"
string(1) "y"
NULL
string(1) "y"
string(1) "y"
NULL
string(1) "y"
string(1) "y"
NULL
string(1) "y"
bool(true)
bool(false)
NULL
bool(false)
bool(true)
NULL
bool(true)
bool(false)
NULL
