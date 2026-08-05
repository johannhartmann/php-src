--TEST--
Native strlen preserves literal string assignment length
--FILE--
<?php
function literal_string_length(): int
{
    $value = 'native string';
    return strlen($value);
}

$value = 'abc';
var_dump(strlen($value));
var_dump(literal_string_length());
?>
--EXPECT--
int(3)
int(13)
