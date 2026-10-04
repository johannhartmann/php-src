--TEST--
Native isset()/empty() of temporaries and if on strings without decoding
--DESCRIPTION--
isset() and empty() of an array element of a temporary, CV or literal
container, and if/while on null, booleans, integers and strings, decide
at the helper's entry; temporaries are released first, their destructors
running as in the VM. Strings, null containers and missing keys keep
stock semantics.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class D { function __destruct() { echo "~D|"; } }
function f() { return ['a' => 1, 'n' => null, 'z' => 0, 's' => '', 'o' => new D, '3' => 3, 'r' => [0]]; }
function t() {
  $o = [isset(f()['a']), isset(f()['n']), empty(f()['z']), empty(f()['s']), isset(f()['o']), isset(f()[3]), isset(f()['3']), empty(f()['r']), isset(f()['missing']), empty(f()['missing']), isset(f()['r'][0])];
  $x = 'abc'; $o[] = isset($x[1]); $n = null; $o[] = isset($n['a']);
  $s = 'str'; if ($s) $o[] = 'truthy'; if (!'0') $o[] = 'zero-falsy'; if (f()['z']) $o[] = 'bad'; while ('' ) {}
  return $o;
}
echo json_encode(t()), "\n";
?>
--EXPECT--
~D|~D|~D|~D|~D|~D|~D|~D|~D|~D|~D|~D|[true,false,true,true,true,true,true,false,false,true,true,true,false,"truthy","zero-falsy"]
