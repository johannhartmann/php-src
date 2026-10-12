--TEST--
isset(), === and !== fused into their branch decide like the VM
--FILE--
<?php
function cv_set($v) { if (isset($v)) { return 'set'; } return 'unset'; }
function cv_undef() { if (isset($u)) { return 'set'; } return 'unset'; }
function cv_ref(&$r) { if (isset($r)) { return 'set'; } return 'unset'; }
function cv_not($v) { if (!isset($v)) { return 'unset'; } return 'set'; }
function id_null($v) { if ($v === null) { return 'null'; } return 'other'; }
function id_false($v) { if (false === $v) { return 'false'; } return 'other'; }
function id_int($v) { if ($v === 42) { return '42'; } return 'other'; }
function id_big($v) { if ($v === 1 << 40) { return 'big'; } return 'other'; }
function id_str($v) { if ($v === 'abc') { return 'abc'; } return 'other'; }
function nid_str($v) { if ($v !== 'abc') { return 'not abc'; } return 'abc'; }
function id_cv($a, $b) { if ($a === $b) { return 'same'; } return 'different'; }
function id_tmp($s) { if (strpos($s, 'x') === false) { return 'no x'; } return 'x'; }
function id_tmp2($s) { if (str_repeat($s, 2) === $s . $s) { return 'eq'; } return 'ne'; }
function id_tmp_tmp($s) { if (strtoupper($s) === strtolower($s)) { return 'eq'; } return 'ne'; }
function id_ref(&$r) { if ($r === 'abc') { return 'abc'; } return 'other'; }
function id_undef() { if ($u === null) { return 'null'; } return 'other'; }
function id_double($v) { if ($v === 1.5) { return '1.5'; } return 'other'; }
function id_arr($v) { if ($v === [1, 2]) { return 'arr'; } return 'other'; }
function dim_lit($a) { if (isset($a['key'])) { return 'set'; } return 'unset'; }
function dim_int($a) { if (isset($a[3])) { return 'set'; } return 'unset'; }
function dim_cv($a, $k) { if (isset($a[$k])) { return 'set'; } return 'unset'; }
function dim_tmp($a, $p, $s) { if (isset($a[$p . $s])) { return 'set'; } return 'unset'; }
function dim_read($a, $k) { if (isset($a[$k])) { return $a[$k]; } return 'none'; }
function dim_read_lit($a) { if (isset($a['key'])) { return $a['key']; } return 'none'; }
function dim_typed(array $a, string $k) { if (isset($a[$k])) { return $a[$k]; } return 'none'; }
function dim_not($a, $k) { if (!isset($a[$k])) { return 'unset'; } return 'set'; }
function dim_ref(&$a, $k) { if (isset($a[$k])) { return $a[$k]; } return 'none'; }
function dim_global($k) { global $G; if (isset($G[$k])) { return $G[$k]; } return 'none'; }
function dim_loop($rows) { $n = 0; foreach ($rows as $row) { if (isset($row['v'])) { $n += $row['v']; } } return $n; }
function dim_literal_container() { if (isset(['a' => 1]['a'])) { return 'set'; } return 'unset'; }
function dim_and($a, $k) { if (isset($a[$k]) && $a[$k] > 1) { return 'big'; } return 'small'; }

set_error_handler(function ($no, $msg) { echo "warning: $msg\n"; return true; });
$G = ['g' => 'global value', 'n' => null];
$long = 'a_runtime_key_longer_than_16_bytes';
$a = ['key' => 'value', 3 => 'three', 'n' => null, $long => 'long', '7' => 'seven', '' => 'empty'];
$o = new ArrayObject(['key' => 'object']);
for ($round = 0; $round < 3; $round++) {
	echo "-- $round\n";
	$r = 1; $rn = null;
	var_dump(cv_set(1), cv_set(null), cv_undef(), cv_ref($r), cv_ref($rn), cv_not(null), cv_not(0));
	$ref = 'abc';
	var_dump(id_null(null), id_null(0), id_null(false), id_false(false), id_false(0), id_int(42), id_int(42.0), id_int('42'));
	var_dump(id_big(1 << 40), id_big(1), id_str('abc'), id_str('ab' . 'c'), id_str(null), nid_str('abd'), nid_str(str_repeat('abc', 1)));
	var_dump(id_cv(1, 1), id_cv('a', 'a'), id_cv([1], [1]), id_cv(1, '1'), id_cv(1.0, 1.0), id_cv(null, false));
	var_dump(id_tmp('abc'), id_tmp('xyz'), id_tmp2('ab'), id_tmp_tmp('1'), id_tmp_tmp('a'), id_ref($ref), id_undef());
	var_dump(id_double(1.5), id_double(1), id_arr([1, 2]), id_arr([2, 1]));
	var_dump(dim_lit($a), dim_lit([]), dim_lit(null), dim_lit('str'), dim_lit($o), dim_lit(['key' => null]));
	var_dump(dim_int($a), dim_int([1, 2, 3, 4]), dim_int([1]), dim_int('abcd'));
	var_dump(dim_cv($a, 'key'), dim_cv($a, 'n'), dim_cv($a, 3), dim_cv($a, '3'), dim_cv($a, '7'), dim_cv($a, 7), dim_cv($a, ''), dim_cv($a, null), dim_cv($a, 3.0), dim_cv($a, $long), dim_cv($a, str_repeat('a_runtime_key_', 1) . 'longer_than_16_bytes'));
	var_dump(dim_cv($o, 'key'), dim_cv(null, 'key'), dim_tmp($a, 'k', 'ey'), dim_tmp($a, 'n', 'o'));
	var_dump(dim_read($a, 'key'), dim_read($a, 'n'), dim_read($a, 'zz'), dim_read($a, 3), dim_read($a, '7'), dim_read($o, 'key'), dim_read_lit($a), dim_read_lit(['key' => [1]]));
	var_dump(dim_typed($a, 'key'), dim_typed($a, 'missing'), dim_typed($a, $long), dim_not($a, 'key'), dim_not($a, 'zz'));
	$copy = $a; $kk = 'key';
	var_dump(dim_ref($copy, 'key'), dim_ref($copy, 'zz'), dim_global('g'), dim_global('n'), dim_global('zz'));
	var_dump(dim_loop([['v' => 1], ['w' => 2], ['v' => 3], [], ['v' => null]]), dim_literal_container(), dim_and(['x' => 2], 'x'), dim_and(['x' => 1], 'x'), dim_and([], 'x'));
}
?>
--EXPECT--
-- 0
string(3) "set"
string(5) "unset"
string(5) "unset"
string(3) "set"
string(5) "unset"
string(5) "unset"
string(3) "set"
string(4) "null"
string(5) "other"
string(5) "other"
string(5) "false"
string(5) "other"
string(2) "42"
string(5) "other"
string(5) "other"
string(3) "big"
string(5) "other"
string(3) "abc"
string(3) "abc"
string(5) "other"
string(7) "not abc"
string(3) "abc"
string(4) "same"
string(4) "same"
string(4) "same"
string(9) "different"
string(4) "same"
string(9) "different"
warning: Undefined variable $u
string(4) "no x"
string(1) "x"
string(2) "eq"
string(2) "eq"
string(2) "ne"
string(3) "abc"
string(4) "null"
string(3) "1.5"
string(5) "other"
string(3) "arr"
string(5) "other"
string(3) "set"
string(5) "unset"
string(5) "unset"
string(5) "unset"
string(3) "set"
string(5) "unset"
string(3) "set"
string(3) "set"
string(5) "unset"
string(3) "set"
warning: Using null as an array offset is deprecated, use an empty string instead
string(3) "set"
string(5) "unset"
string(3) "set"
string(3) "set"
string(3) "set"
string(3) "set"
string(3) "set"
string(3) "set"
string(3) "set"
string(3) "set"
string(3) "set"
string(3) "set"
string(5) "unset"
string(3) "set"
string(5) "unset"
string(5) "value"
string(4) "none"
string(4) "none"
string(5) "three"
string(5) "seven"
string(6) "object"
string(5) "value"
array(1) {
  [0]=>
  int(1)
}
string(5) "value"
string(4) "none"
string(4) "long"
string(3) "set"
string(5) "unset"
string(5) "value"
string(4) "none"
string(12) "global value"
string(4) "none"
string(4) "none"
int(4)
string(3) "set"
string(3) "big"
string(5) "small"
string(5) "small"
-- 1
string(3) "set"
string(5) "unset"
string(5) "unset"
string(3) "set"
string(5) "unset"
string(5) "unset"
string(3) "set"
string(4) "null"
string(5) "other"
string(5) "other"
string(5) "false"
string(5) "other"
string(2) "42"
string(5) "other"
string(5) "other"
string(3) "big"
string(5) "other"
string(3) "abc"
string(3) "abc"
string(5) "other"
string(7) "not abc"
string(3) "abc"
string(4) "same"
string(4) "same"
string(4) "same"
string(9) "different"
string(4) "same"
string(9) "different"
warning: Undefined variable $u
string(4) "no x"
string(1) "x"
string(2) "eq"
string(2) "eq"
string(2) "ne"
string(3) "abc"
string(4) "null"
string(3) "1.5"
string(5) "other"
string(3) "arr"
string(5) "other"
string(3) "set"
string(5) "unset"
string(5) "unset"
string(5) "unset"
string(3) "set"
string(5) "unset"
string(3) "set"
string(3) "set"
string(5) "unset"
string(3) "set"
warning: Using null as an array offset is deprecated, use an empty string instead
string(3) "set"
string(5) "unset"
string(3) "set"
string(3) "set"
string(3) "set"
string(3) "set"
string(3) "set"
string(3) "set"
string(3) "set"
string(3) "set"
string(3) "set"
string(3) "set"
string(5) "unset"
string(3) "set"
string(5) "unset"
string(5) "value"
string(4) "none"
string(4) "none"
string(5) "three"
string(5) "seven"
string(6) "object"
string(5) "value"
array(1) {
  [0]=>
  int(1)
}
string(5) "value"
string(4) "none"
string(4) "long"
string(3) "set"
string(5) "unset"
string(5) "value"
string(4) "none"
string(12) "global value"
string(4) "none"
string(4) "none"
int(4)
string(3) "set"
string(3) "big"
string(5) "small"
string(5) "small"
-- 2
string(3) "set"
string(5) "unset"
string(5) "unset"
string(3) "set"
string(5) "unset"
string(5) "unset"
string(3) "set"
string(4) "null"
string(5) "other"
string(5) "other"
string(5) "false"
string(5) "other"
string(2) "42"
string(5) "other"
string(5) "other"
string(3) "big"
string(5) "other"
string(3) "abc"
string(3) "abc"
string(5) "other"
string(7) "not abc"
string(3) "abc"
string(4) "same"
string(4) "same"
string(4) "same"
string(9) "different"
string(4) "same"
string(9) "different"
warning: Undefined variable $u
string(4) "no x"
string(1) "x"
string(2) "eq"
string(2) "eq"
string(2) "ne"
string(3) "abc"
string(4) "null"
string(3) "1.5"
string(5) "other"
string(3) "arr"
string(5) "other"
string(3) "set"
string(5) "unset"
string(5) "unset"
string(5) "unset"
string(3) "set"
string(5) "unset"
string(3) "set"
string(3) "set"
string(5) "unset"
string(3) "set"
warning: Using null as an array offset is deprecated, use an empty string instead
string(3) "set"
string(5) "unset"
string(3) "set"
string(3) "set"
string(3) "set"
string(3) "set"
string(3) "set"
string(3) "set"
string(3) "set"
string(3) "set"
string(3) "set"
string(3) "set"
string(5) "unset"
string(3) "set"
string(5) "unset"
string(5) "value"
string(4) "none"
string(4) "none"
string(5) "three"
string(5) "seven"
string(6) "object"
string(5) "value"
array(1) {
  [0]=>
  int(1)
}
string(5) "value"
string(4) "none"
string(4) "long"
string(3) "set"
string(5) "unset"
string(5) "value"
string(4) "none"
string(12) "global value"
string(4) "none"
string(4) "none"
int(4)
string(3) "set"
string(3) "big"
string(5) "small"
string(5) "small"
