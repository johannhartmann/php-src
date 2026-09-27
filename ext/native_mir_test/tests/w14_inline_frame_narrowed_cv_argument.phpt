--TEST--
Native x64 inline call frames with a CV narrowed by a comparison
--DESCRIPTION--
After if ($m == 0), $m is a pi of the parameter without a machine
definition of its own. An inline call frame reads such a CV argument from
its slot.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function add($a, $b = 0) { return $a + $b; }
function narrowed($m, $n) {
    if ($m == 0) {
        return 1;
    }
    return add($m - 1, $m) . "/" . add($m, $n);
}
for ($i = 0; $i < 3; $i++) {
    var_dump(narrowed(0, 1), narrowed(3, 4), narrowed(2.5, 1), narrowed("7", 2));
}
?>
--EXPECT--
int(1)
string(3) "5/7"
string(5) "4/3.5"
string(4) "13/9"
int(1)
string(3) "5/7"
string(5) "4/3.5"
string(4) "13/9"
int(1)
string(3) "5/7"
string(5) "4/3.5"
string(4) "13/9"
