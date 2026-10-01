--TEST--
Native frameless internal calls of two and three arguments
--DESCRIPTION--
FRAMELESS_ICALL_2 and _3 of defined arguments call the frameless handler
with the dereferenced arguments and release temporaries afterwards in
argument order, as the VM handlers do; an undefined CV still warns before
the call through the general form.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class S { function __construct(public $n) {} function __toString(): string { return "s{$this->n}"; } function __destruct() { echo "~{$this->n} "; } }
function run($i) {
    $hay = "a-b-c-$i"; $r = 'b'; $ref = &$r;
    $o = [];
    $o[] = str_replace('-', '+', $hay);                          // three: literal, literal, CV
    $o[] = str_replace($ref, strtoupper($r), $hay . '!');        // reference, temporary, temporary
    $o[] = substr($hay, 2) . substr($hay, 1, $i + 1);            // two and three
    $o[] = strpos($hay, 'c') . (int) str_contains($hay, (string) $i);
    $o[] = implode('|', [new S($i), new S($i + 10)]);             // temporaries with destructors
    $o[] = in_array(new S(7), [1, 2], false) ? 'y' : 'n';
    $o[] = array_key_exists('k', ['k' => $i]) ? 'k' : '-';
    $o[] = min($i, 2) . max($i, 1, -1);
    $o[] = @substr($undef, 0, 1) . '|' . str_pad((string) $i, 3, '0', STR_PAD_LEFT);
    return implode(' ', $o);
}
for ($i = 0; $i < 3; $i++) { echo run($i), "\n"; }
echo str_replace('a', $missing, 'abc'), "\n";
?>
--EXPECTF--
~0 ~10 
Notice: Object of class S could not be converted to int in %s on line 11
~7 a+b+c+0 a-B-c-0! b-c-0- 41 s0|s10 y k 01 |000
~1 ~11 
Notice: Object of class S could not be converted to int in %s on line 11
~7 a+b+c+1 a-B-c-1! b-c-1-b 41 s1|s11 y k 11 |001
~2 ~12 
Notice: Object of class S could not be converted to int in %s on line 11
~7 a+b+c+2 a-B-c-2! b-c-2-b- 41 s2|s12 y k 22 |002

Warning: Undefined variable $missing in %s on line 18

Deprecated: str_replace(): Passing null to parameter #2 ($replace) of type array|string is deprecated in %s on line 18
bc
