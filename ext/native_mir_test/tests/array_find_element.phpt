--TEST--
Native array probes that cannot decide a key complete it in C before the helper
--FILE--
<?php
function read($a, $k) { return $a[$k]; }
function read_tmp($a, $p, $s) { return $a[$p . $s]; }
function coalesce($a, $k) { return $a[$k] ?? 'default'; }
function has($a, $k) { return isset($a[$k]); }
function has_tmp($a, $p, $s) { return isset($a[$p . $s]); }
function is_empty($a, $k) { return empty($a[$k]); }
function literal_long($a) { return [$a['a_literal_key_longer_than_16'] ?? 'none', isset($a['a_literal_key_longer_than_16'])]; }
function branch($a, $k) { if (isset($a[$k])) { return 'set'; } return 'unset'; }
function known(array $a, $k) { return [$a[$k] ?? 'none', isset($a[$k])]; }
function by_ref(&$a, &$k) { return [$a[$k] ?? 'none', isset($a[$k]), $a[$k]]; }

set_error_handler(function ($no, $msg) { echo "warning: $msg\n"; return true; });

$long = 'a_runtime_key_longer_than_16_bytes';
$a = [
	$long => 'long value',
	'short' => 'short value',
	'a_literal_key_longer_than_16' => 'literal value',
	'null_value_key_longer_than_16' => null,
	'zero_value_key_longer_than_16' => 0,
	42 => 'forty-two',
	'-7' => 'minus seven',
	'007' => 'string 007',
];
for ($round = 0; $round < 3; $round++) {
	echo "-- round $round\n";
	// Fresh strings: equal content, another pointer, no hash yet.
	$fresh = str_repeat('a_runtime_key_longer', 1) . '_than_16_bytes';
	var_dump(read($a, $fresh), read_tmp($a, 'a_runtime_key_', 'longer_than_16_bytes'));
	var_dump(coalesce($a, $fresh), has($a, $fresh), has_tmp($a, 'sh', 'ort'));
	var_dump(is_empty($a, $fresh), is_empty($a, 'zero_value_key_' . 'longer_than_16'));
	var_dump(has($a, 'null_value_key_' . 'longer_than_16'), coalesce($a, 'null_value_key_' . 'longer_than_16'));
	var_dump(literal_long($a), branch($a, $fresh), branch($a, 'missing_key_' . 'longer_than_16'));
	// Numeric strings name integer keys; others stay strings.
	var_dump(read($a, '4' . '2'), has($a, '-' . '7'), coalesce($a, '0' . '07'), has($a, '7'));
	// Missing keys.
	var_dump(coalesce($a, 'missing_key_' . 'longer_than_16'), has($a, 'missing'), read($a, 'missing_key_' . 'longer_than_16'));
	// Null and undefined containers, scalar containers.
	var_dump(coalesce(null, $fresh), has(null, $fresh), has('string', $fresh), coalesce(42, $fresh));
	var_dump(read('string', '1'), has('string', '1'));
	// Typed array parameter, references.
	var_dump(known($a, $fresh));
	$k = 'a_runtime_key_longer_than' . '_16_bytes';
	var_dump(by_ref($a, $k));
	// Other key types.
	var_dump(has($a, null), coalesce($a, 42.0), has($a, true));
	// ArrayAccess.
	$o = new ArrayObject([$long => 'object value']);
	var_dump(coalesce($o, $fresh), has($o, $fresh), read($o, $fresh));
	// The symbol table holds INDIRECT elements.
	$GLOBALS['global_key_longer_than_16_bytes'] = 'global value';
	var_dump(has($GLOBALS, 'global_key_' . 'longer_than_16_bytes'), coalesce($GLOBALS, 'global_key_' . 'longer_than_16_bytes'));
}
?>
--EXPECT--
-- round 0
string(10) "long value"
string(10) "long value"
string(10) "long value"
bool(true)
bool(true)
bool(false)
bool(true)
bool(false)
string(7) "default"
array(2) {
  [0]=>
  string(13) "literal value"
  [1]=>
  bool(true)
}
string(3) "set"
string(5) "unset"
string(9) "forty-two"
bool(true)
string(10) "string 007"
bool(false)
warning: Undefined array key "missing_key_longer_than_16"
string(7) "default"
bool(false)
NULL
string(7) "default"
bool(false)
bool(false)
string(7) "default"
string(1) "t"
bool(true)
array(2) {
  [0]=>
  string(10) "long value"
  [1]=>
  bool(true)
}
array(3) {
  [0]=>
  string(10) "long value"
  [1]=>
  bool(true)
  [2]=>
  string(10) "long value"
}
warning: Using null as an array offset is deprecated, use an empty string instead
bool(false)
string(9) "forty-two"
bool(false)
string(12) "object value"
bool(true)
string(12) "object value"
bool(true)
string(12) "global value"
-- round 1
string(10) "long value"
string(10) "long value"
string(10) "long value"
bool(true)
bool(true)
bool(false)
bool(true)
bool(false)
string(7) "default"
array(2) {
  [0]=>
  string(13) "literal value"
  [1]=>
  bool(true)
}
string(3) "set"
string(5) "unset"
string(9) "forty-two"
bool(true)
string(10) "string 007"
bool(false)
warning: Undefined array key "missing_key_longer_than_16"
string(7) "default"
bool(false)
NULL
string(7) "default"
bool(false)
bool(false)
string(7) "default"
string(1) "t"
bool(true)
array(2) {
  [0]=>
  string(10) "long value"
  [1]=>
  bool(true)
}
array(3) {
  [0]=>
  string(10) "long value"
  [1]=>
  bool(true)
  [2]=>
  string(10) "long value"
}
warning: Using null as an array offset is deprecated, use an empty string instead
bool(false)
string(9) "forty-two"
bool(false)
string(12) "object value"
bool(true)
string(12) "object value"
bool(true)
string(12) "global value"
-- round 2
string(10) "long value"
string(10) "long value"
string(10) "long value"
bool(true)
bool(true)
bool(false)
bool(true)
bool(false)
string(7) "default"
array(2) {
  [0]=>
  string(13) "literal value"
  [1]=>
  bool(true)
}
string(3) "set"
string(5) "unset"
string(9) "forty-two"
bool(true)
string(10) "string 007"
bool(false)
warning: Undefined array key "missing_key_longer_than_16"
string(7) "default"
bool(false)
NULL
string(7) "default"
bool(false)
bool(false)
string(7) "default"
string(1) "t"
bool(true)
array(2) {
  [0]=>
  string(10) "long value"
  [1]=>
  bool(true)
}
array(3) {
  [0]=>
  string(10) "long value"
  [1]=>
  bool(true)
  [2]=>
  string(10) "long value"
}
warning: Using null as an array offset is deprecated, use an empty string instead
bool(false)
string(9) "forty-two"
bool(false)
string(12) "object value"
bool(true)
string(12) "object value"
bool(true)
string(12) "global value"
