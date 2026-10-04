--TEST--
Native call entry under strict types falls back for non-matching arguments
--EXTENSIONS--
native_mir_test
--FILE--
<?php
declare(strict_types=1);
function typed(int $i, ?string $s = null) { return "$i|" . var_export($s, true); }
for ($round = 0; $round < 3; $round++) {
    echo typed(1), ' ', typed(2, 's'), "\n";
    try { echo typed('3'), "\n"; } catch (TypeError $e) { echo 'TypeError: ', $e->getMessage(), "\n"; }
    try { echo typed(4, 5), "\n"; } catch (TypeError $e) { echo 'TypeError: ', $e->getMessage(), "\n"; }
}
--EXPECTF--
1|NULL 2|'s'
TypeError: typed(): Argument #1 ($i) must be of type int, string given, called in %s on line 6
TypeError: typed(): Argument #2 ($s) must be of type ?string, int given, called in %s on line 7
1|NULL 2|'s'
TypeError: typed(): Argument #1 ($i) must be of type int, string given, called in %s on line 6
TypeError: typed(): Argument #2 ($s) must be of type ?string, int given, called in %s on line 7
1|NULL 2|'s'
TypeError: typed(): Argument #1 ($i) must be of type int, string given, called in %s on line 6
TypeError: typed(): Argument #2 ($s) must be of type ?string, int given, called in %s on line 7
