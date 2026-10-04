--TEST--
Native internal calls with directly transferred arguments
--DESCRIPTION--
A positional by-value SEND of a CV, temporary or literal to an internal
function moves into the call frame directly while the bound function takes
the parameter by value, and the call completes without the observer and
bailout protocol. References, undefined variables, by-reference and
prefer-reference parameters, named and extra arguments, exceptions and
destructors run by argument cleanup keep their semantics.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class NativeTracked {
    public function __construct(public $name) {}
    public function __destruct() { echo "destroy {$this->name}\n"; }
    public function __toString(): string { return $this->name; }
}

function native_internal($n)
{
    $out = [];
    $text = str_repeat('ab', 2);
    $list = [3, 1, 2];
    $target = 'xyz';
    $alias = &$target;
    for ($i = 0; $i < $n; $i++) {
        $out[] = strlen($text);
        $out[] = str_replace('a', 'A', $text . $i);
        $out[] = substr('literal', 1, 3);
        $out[] = strtoupper($alias);
        $out[] = implode(',', $list);
        $out[] = in_array($i, $list, true) ? 'in' : 'out';
        $out[] = function_exists('native_internal') ? 'yes' : 'no';
        $copy = $list;
        sort($copy);
        $out[] = implode(',', $copy) . '/' . implode(',', $list);
        $out[] = preg_match('/(b)/', $text, $matches) . ':' . $matches[1];
        $out[] = str_pad(string: 'p', length: 3, pad_string: '-');
        $out[] = max(1, $i, 2, $n);
        $out[] = sprintf('%s-%d', new NativeTracked("s$i"), $i);
    }
    try {
        str_repeat('x', -1);
    } catch (ValueError $e) {
        $out[] = get_class($e);
    }
    $out[] = @strlen($undefined);
    array_multisort($list);
    $out[] = implode(',', $list);
    strtolower('UNUSED');
    return $out;
}
echo implode("\n", native_internal(2)), "\n";
?>
--EXPECT--
destroy s0
destroy s1
4
AbAb0
ite
XYZ
3,1,2
out
yes
1,2,3/3,1,2
1:b
p--
2
s0-0
4
AbAb1
ite
XYZ
3,1,2
in
yes
1,2,3/3,1,2
1:b
p--
2
s1-1
ValueError
0
1,2,3
