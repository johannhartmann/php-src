--TEST--
Native direct internal calls decide their source finish from per-site flags
--DESCRIPTION--
The direct internal-call descriptor records at build time whether its Do
is a call with an unused or temporary result, and the internal-call cell
whether its function is neither deprecated, nodiscard nor a trampoline.
Unused, temporary and CV results, callable conversions, deprecated
functions, methods and undefined arguments keep their PHP behaviour.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
date.timezone=UTC
--FILE--
<?php
function sites($s, $n) {
    $out = [];
    for ($i = 0; $i < $n; $i++) {
        strlen($s);
        $out[] = strlen($s) + $i;
        $len = str_repeat($s, 2);
        $out[] = $len;
        $f = strlen(...);
        $out[] = $f($s);
        $g = str_pad(...);
        $out[] = $g($s, 6, '-');
        $out[] = @strftime('%Y', 0) !== '' ? 'year' : 'none';
        $a = new ArrayObject([1, 2, $i]);
        $a->count();
        $out[] = $a->count();
        $out[] = max(1, $i, ...[3]);
    }
    return $out;
}
function deprecated_sites() {
    $r = strftime('%Y', 86400 * 400);
    strftime('%m', 0);
    return $r;
}
function undef_arg() {
    return strlen($missing ?? 'x') + strlen($nope);
}
var_dump(sites('ab', 2));
var_dump(deprecated_sites());
try { var_dump(undef_arg()); } catch (Throwable $e) { echo get_class($e), "\n"; }
?>
--EXPECTF--
array(14) {
  [0]=>
  int(2)
  [1]=>
  string(4) "abab"
  [2]=>
  int(2)
  [3]=>
  string(6) "ab----"
  [4]=>
  string(4) "year"
  [5]=>
  int(3)
  [6]=>
  int(3)
  [7]=>
  int(3)
  [8]=>
  string(4) "abab"
  [9]=>
  int(2)
  [10]=>
  string(6) "ab----"
  [11]=>
  string(4) "year"
  [12]=>
  int(3)
  [13]=>
  int(3)
}

Deprecated: Function strftime() is deprecated since 8.1, use IntlDateFormatter::format() instead in %s on line 22

Deprecated: Function strftime() is deprecated since 8.1, use IntlDateFormatter::format() instead in %s on line 23
string(4) "1971"

Warning: Undefined variable $nope in %s on line 27

Deprecated: strlen(): Passing null to parameter #1 ($string) of type string is deprecated in %s on line 27
int(1)
