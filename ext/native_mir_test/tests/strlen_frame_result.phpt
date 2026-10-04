--TEST--
Native x64 strlen of a string CV with a frame-only result
--DESCRIPTION--
strlen() of a string parameter whose result only feeds RETURN has no machine
result. The x64 guarded strlen publishes the length to the frame slot; other
operand types keep the helper.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
function len(string $s) { return strlen($s); }
function len_mixed($s) { return strlen($s); }
function len_ref(string &$s) { return strlen($s); }
function len_loop(array $a) { $r = 0; foreach ($a as $s) { $r += strlen($s); } return $r; }
function len_coerce(string $s) { return strlen($s); }
for ($i = 0; $i < 3; $i++) {
    $x = "abc$i";
    var_dump(len('native'), len(''), len_mixed("xy"), len_ref($x), len_loop(['a', 'bb', 'ccc']), len_coerce(12345));
    try { var_dump(len_mixed([])); } catch (TypeError $e) { echo $e->getMessage(), "\n"; }
    var_dump(len_mixed(3.5), len_mixed(null ?? 'n'));
}
?>
--EXPECT--
int(6)
int(0)
int(2)
int(4)
int(6)
int(5)
strlen(): Argument #1 ($string) must be of type string, array given
int(3)
int(1)
int(6)
int(0)
int(2)
int(4)
int(6)
int(5)
strlen(): Argument #1 ($string) must be of type string, array given
int(3)
int(1)
int(6)
int(0)
int(2)
int(4)
int(6)
int(5)
strlen(): Argument #1 ($string) must be of type string, array given
int(3)
int(1)
