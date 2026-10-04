--TEST--
Native TYPE_CHECK of temporaries through an EncodeGen snippet
--DESCRIPTION--
is_*() and === false of call results and other temporaries inline when
another owner keeps them alive; a last owner takes the helper, which
releases the value.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class O {}
function arr() { return [1]; }
function str() { return str_repeat('s', 3); }
function obj() { static $o; return $o ??= new O; }
function fresh() { return new O; }
function maybe($i) { return $i % 2 ? false : obj(); }
function tmp_checks($i)
{
    $out = [is_array(arr()), is_string(str()), is_null(null ?? $i), maybe($i) === false,
        maybe($i) !== false, is_object(fresh()), is_array([$i]), is_string("v$i"), is_int($i + 1)];
    if (is_array(arr())) { $out[] = 'arr'; }
    return $out;
}
for ($i = 0; $i < 4; $i++) { echo json_encode(tmp_checks($i)), "\n"; }
?>
--EXPECTF--
[true,true,false,false,true,true,true,true,true,"arr"]
[true,true,false,true,false,true,true,true,true,"arr"]
[true,true,false,false,true,true,true,true,true,"arr"]
[true,true,false,true,false,true,true,true,true,"arr"]
