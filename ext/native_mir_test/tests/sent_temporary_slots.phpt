--TEST--
Temporaries sent to a call keep their slot until the call's DO
--DESCRIPTION--
OPcache no longer reuses the slot of a temporary sent to a call before the
call's DO, so f($i - 1, $s - 1) keeps both arguments and a native direct
call can transfer them at DO. An exception thrown after a temporary was
sent releases that temporary.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function f($a, $b) { return "$a|$b"; }
function two_temporaries($n) {
    $s = "";
    for ($i = 0; $i < $n; $i++) {
        $s = f($i - 1, $i * 2 - 1);
    }
    return $s;
}
function concatenated($x, $y) { return f($x . $y, $y . $x); }
function throws_after_send($x, $y, $z) {
    try {
        return f($x . $y, $z % 0);
    } catch (DivisionByZeroError $e) {
        return "caught";
    }
}
for ($round = 0; $round < 3; $round++) {
    echo two_temporaries(5), " ", concatenated("a", str_repeat("b", 2)), " ",
        throws_after_send("a", str_repeat("b", 3), 5), "\n";
}
?>
--EXPECT--
3|7 abb|bba caught
3|7 abb|bba caught
3|7 abb|bba caught
