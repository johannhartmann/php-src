--TEST--
String interpolation pieces stored without decoding
--DESCRIPTION--
ROPE_INIT and ROPE_ADD store string pieces from literals, CVs and
temporaries without decoding; references, numbers, objects with
__toString and undefined variables keep the general path.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class S { function __toString(): string { return 'obj'; } }
function render($name, $n) {
    $r = &$name; $o = new S; $f = 1.5;
    return ["<a href=\"$name\">{$n}</a>", "$r-$r", "x" . strtoupper($name) . "{$o}", "{$f}|{$n}|" . str_repeat('z', $n) . "|$name"];
}
for ($i = 0; $i < 3; $i++) { echo json_encode(render("n$i", $i)), "\n"; }
echo "[{$undefined}]\n";
?>
--EXPECTF--
["<a href=\"n0\">0<\/a>","n0-n0","xN0obj","1.5|0||n0"]
["<a href=\"n1\">1<\/a>","n1-n1","xN1obj","1.5|1|z|n1"]
["<a href=\"n2\">2<\/a>","n2-n2","xN2obj","1.5|2|zz|n2"]

Warning: Undefined variable $undefined in %s on line 8
[]
