--TEST--
A CV bound to a reference keeps it when a helper reads the value assigned to it
--FILE--
<?php
function g($a) { return $a; }
function by_call($buf) {
    global $user;
    $user = g($buf);
    var_dump($user == null, $user !== "a" && $user !== "b");
}
function by_warning($buf) {
    global $user;
    $user = $buf[1];
    var_dump($user == null, $user + 1);
}
function by_static($buf) {
    static $count;
    $count = g($buf);
    return $count == 2;
}
function by_ref(&$target, $buf) {
    $target = g($buf);
    return $target == 3;
}
for ($i = 0; $i < 3; $i++) {
    by_call($i);
    var_dump($user);
    by_warning($i);
    var_dump($user);
    var_dump(by_static($i));
    $x = 0;
    var_dump(by_ref($x, $i + 1), $x);
}
?>
--EXPECTF--
bool(true)
bool(true)
int(0)

Warning: Trying to access array offset on int in %s on line 10
bool(true)
int(1)
NULL
bool(false)
bool(false)
int(1)
bool(false)
bool(true)
int(1)

Warning: Trying to access array offset on int in %s on line 10
bool(true)
int(1)
NULL
bool(false)
bool(false)
int(2)
bool(false)
bool(true)
int(2)

Warning: Trying to access array offset on int in %s on line 10
bool(true)
int(1)
NULL
bool(true)
bool(true)
int(3)
