--TEST--
Native count result remains defined across a loop PHI
--INI--
opcache.enable=1
opcache.enable_cli=1
--FILE--
<?php
function gh12512_native_loop_phi($values): bool
{
	$count = count($values);
	do {
		$previous = $count;
		$count = count($values);
	} while ($previous !== $count);
	return true;
}

function gh12512_native_count_same_cv($value): int
{
	$value = count($value);
	return $value;
}

var_dump(gh12512_native_loop_phi([1, 2]));
var_dump(gh12512_native_count_same_cv([1, 2, 3]));
?>
--EXPECT--
bool(true)
int(3)
