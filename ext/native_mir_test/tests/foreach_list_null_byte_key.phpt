--TEST--
Native: foreach with list() destructuring keeps array keys that start with a NUL byte
--DESCRIPTION--
Only object property tables hold mangled names. An array key such as
"\0Class\0name" is an ordinary string and reaches the loop key unchanged.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function keys_plain(array $map): string { $o = []; foreach ($map as $key => $v) { $o[] = bin2hex($key); } return implode(' ', $o); }
function keys_list(array $map): string { $o = []; foreach ($map as $key => [$scope, $name]) { $o[] = bin2hex($key) . ':' . ($key === "\0$scope\0$name" ? 'eq' : 'ne'); } return implode(' ', $o); }
class Holder { private $hidden = 1; protected $prot = 2; public $pub = 3;
    function keys(): string { $o = []; foreach ($this as $key => $v) { $o[] = $key; } return implode(' ', $o); } }
$map = ["\0Klass\0prop" => ['Klass', 'prop'], "\0*\0prot" => ['*', 'prot'], 'plain' => ['', 'plain']];
echo keys_plain($map), "\n", keys_list($map), "\n", (new Holder)->keys(), "\n";
--EXPECT--
004b6c6173730070726f70 002a0070726f74 706c61696e
004b6c6173730070726f70:eq 002a0070726f74:eq 706c61696e:ne
hidden prot pub
