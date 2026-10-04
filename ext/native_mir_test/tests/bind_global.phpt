--TEST--
Native BIND_GLOBAL through the run-time cached symbol table bucket
--DESCRIPTION--
global $x binds a CV to the global through the bucket offset the run-time
cache keeps, as the VM does. Globals that do not exist yet, globals of the
main script's CVs (INDIRECT buckets), a CV that already holds a counted
value, rebinding, unset(), $GLOBALS writes and table growth keep stock
semantics.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
$main_cv = 'main';
function read_main() { global $main_cv; return $main_cv; }
function write_main($v) { global $main_cv; $main_cv = $v; }
function counter() { global $count; $count = ($count ?? 0) + 1; return $count; }
function late() { global $late_global; return $late_global; }
function preset() { $x = str_repeat('p', 3); global $x; return $x; }
function twice() { global $count; global $count; return $count; }
function unset_it() { global $count; unset($count); return isset($count) ? 'set' : 'unset'; }
function via_globals() { $GLOBALS['count'] = 100; global $count; return $count; }
function ref_identity() { global $main_cv; $r = &$main_cv; $r = 'changed'; return $GLOBALS['main_cv']; }
function grow() { for ($i = 0; $i < 200; $i++) { $GLOBALS["g$i"] = $i; } global $g150; return $g150; }
$out = [];
for ($round = 0; $round < 3; $round++) {
    $out[] = read_main();
    write_main("m$round");
    $out[] = $main_cv;
    $out[] = counter();
    $out[] = var_export(late(), true);
    $late_global = "late$round";
    $out[] = late();
    $out[] = preset();
    $out[] = twice();
    $out[] = unset_it();
    $out[] = counter();
    $out[] = via_globals();
    $out[] = ref_identity();
    $main_cv = 'main';
    $out[] = grow();
    $out[] = counter();
}
echo json_encode($out), "\n";
?>
--EXPECT--
["main","m0",1,"NULL","late0",null,1,"unset",2,100,"changed",150,101,"main","m1",102,"'late0'","late1",null,102,"unset",103,100,"changed",150,101,"main","m2",102,"'late1'","late2",null,102,"unset",103,100,"changed",150,101]
