--TEST--
Native: unoptimized loops branch on register scalars
--DESCRIPTION--
Without OPcache's optimizer an ASSIGN keeps a boxed COPY of a scalar
operation's result, which the plan refines to the scalar, and a loop laid
out behind a leading JMP lists the Pi copies of the branch that reads it
before that COPY. The adaptor lowers such a Pi copy as a machine copy. A
branch on a register isset() or empty() result replaces the value branch's
helper and observes no frame: its materializations only end their uses.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=0
--FILE--
<?php
function cmp_assign_op(int $n): int { $sum = 0; for ($index = 0; $index < $n; $index++) { $value = $index + 1; if ($value > 3) { $sum += 2; } } return $sum; }
function cmp_else(int $n): int { $sum = 0; for ($index = 0; $index < $n; $index++) { $value = $index + 1; if ($value > 3) { echo ""; } else { $sum--; } } return $sum; }
function empty_assign_op(int $n): int { $sum = 0; for ($index = 0; $index < $n; $index++) { $value = $index + 1; if (empty($value)) { $sum += 2; } } return $sum; }
function isset_assign_op(int $n): int { $sum = 0; for ($index = 0; $index < $n; $index++) { $value = $index + 1; if (isset($value)) { $sum += 2; } } return $sum; }
function empty_else(int $n): int { $sum = 0; for ($index = 0; $index < $n; $index++) { $value = $index & 1; if (empty($value)) { $sum += 2; } else { $sum += 1; } } return $sum; }
function chain(int $n): int { $sum = 0; for ($index = 0; $index < $n; $index++) { $value = $index & 1; if (isset($value) && !empty($value)) { $sum++; } if (!isset($value) || empty($value)) { $sum += 2; } } return $sum; }
function pi_copy(int $n): int { $sum = 0; for ($index = 0; $index < $n; $index++) { $value = $index * 3; if ($value > 30) { $sum = $value; } } return $sum; }
var_dump(cmp_assign_op(50), cmp_else(50), empty_assign_op(50), isset_assign_op(50), empty_else(50), chain(50), pi_copy(50));
?>
--EXPECT--
int(94)
int(-3)
int(0)
int(100)
int(75)
int(75)
int(147)
