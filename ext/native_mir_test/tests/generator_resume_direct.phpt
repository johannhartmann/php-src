--TEST--
Native generator resumes enter the frame directly
--DESCRIPTION--
zend_generator_resume() of a generator running native code enters its
frame without the general activation: values, send(), throw(), exceptions
leaving the body, finally blocks, delegation and early destruction behave
as in the VM.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function g() { try { yield 1; throw new Exception("boom"); } finally { echo "fin\n"; } }
try { foreach (g() as $v) echo $v, "\n"; } catch (Exception $e) { echo "caught ", $e->getMessage(), "\n"; }
function h() { $x = yield 1; echo "got $x\n"; try { yield 2; } catch (Exception $e) { echo "in ", $e->getMessage(), "\n"; yield 3; } }
$g = h(); echo $g->current(), "\n"; echo $g->send("s"), "\n"; echo $g->throw(new Exception("t")), "\n"; $g->next(); var_dump($g->valid());
function inner() { yield 1; throw new LogicException("inner"); }
function outer() { try { yield from inner(); } catch (LogicException $e) { echo "outer caught\n"; yield 9; } }
foreach (outer() as $k => $v) echo "$k=>$v\n";
$gen = (function () { yield 1; yield 2; })(); foreach ($gen as $v) { echo $v; break; } unset($gen); echo "\nend\n";
?>
--EXPECT--
1
fin
caught boom
1
got s
2
in t
3
bool(false)
0=>1
outer caught
0=>9
1
end
