--TEST--
A conditionally declared function calls another one declared later (OPcache)
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
$mode = 1;
if ($mode) {
    function helper($b) { return "h$b"; }
}
if ($mode) {
    function caller($b) { return helper($b) . "!"; }
}
if ($mode) {
    function outer($b) { return inner($b) . "?"; }
    function inner($b) { return "i$b"; }
    echo outer(1), "\n";
}
for ($i = 0; $i < 3; $i++) {
    echo caller($i), "\n";
}
?>
--EXPECTF--
i1?
h0!
h1!
h2!
