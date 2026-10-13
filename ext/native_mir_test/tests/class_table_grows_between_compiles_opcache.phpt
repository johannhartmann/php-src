--TEST--
class_alias() grows the class table between compiles of a request (OPcache)
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function f1($x) { return $x + 1; }
function f2($x) { return f3($x) + 2; }
function f3($x) { return $x + 3; }
trait T { function t($x) { return $x + 7; } }
class K { use T; function a($x) { return $this->b($x) + 1; } function b($x) { return $x; } static function s($x) { return f3($x); } }
echo f1(1), "\n";
for ($i = 0; $i < 3000; $i++) { class_alias('K', "Alias$i"); }
echo f2(1), "\n";
echo K::s(4), "\n";
$k = new K; echo $k->a(5), " ", $k->t(1), "\n";
echo Alias2999::s(1), "\n";
?>
--EXPECT--
2
6
7
6 8
4
