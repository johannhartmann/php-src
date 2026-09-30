--TEST--
Native !, (bool), strlen() and simple casts without decoding
--DESCRIPTION--
!, (bool) and strlen() of null, booleans, integers and strings, and (int),
(string) and (array) casts that cannot warn, decide at the helper's entry.
Doubles and NAN, arrays, objects and undefined variables keep stock
semantics.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function s($x) { return $x; }
function t() {
  $vals = [null, false, true, 0, 1, -5, '', '0', '00', 'a', ' 12abc', '1e3', 1.5, NAN, [], [1], new stdClass];
  $out = [];
  foreach ($vals as $v) {
    $r = [!$v, (bool) $v, !s($v), (bool) s($v)];
    if (!is_object($v) && !is_array($v)) { $r[] = @(int) $v; $r[] = @(string) $v; } else { $r[] = (array) $v; }
    if (is_string($v)) { $r[] = strlen($v); $r[] = strlen(s($v) . 'x'); }
    $out[] = $r;
  }
  $u = @!$undef; $out[] = [$u, (int) '9223372036854775808', (string) PHP_INT_MAX];
  return $out;
}
echo json_encode(t(), JSON_PARTIAL_OUTPUT_ON_ERROR), "\n";
?>
--EXPECTF--

Warning: unexpected NAN value was coerced to bool in %s on line %d

Warning: unexpected NAN value was coerced to bool in %s on line %d

Warning: unexpected NAN value was coerced to bool in %s on line %d

Warning: unexpected NAN value was coerced to bool in %s on line %d
[[true,false,true,false,0,""],[true,false,true,false,0,""],[false,true,false,true,1,"1"],[true,false,true,false,0,"0"],[false,true,false,true,1,"1"],[false,true,false,true,-5,"-5"],[true,false,true,false,0,"",0,1],[true,false,true,false,0,"0",1,2],[false,true,false,true,0,"00",2,3],[false,true,false,true,0,"a",1,2],[false,true,false,true,12," 12abc",6,7],[false,true,false,true,1000,"1e3",3,4],[false,true,false,true,1,"1.5"],[false,true,false,true,0,"NAN"],[true,false,true,false,[]],[false,true,false,true,[1]],[false,true,false,true,[]],[true,9223372036854775807,"9223372036854775807"]]
