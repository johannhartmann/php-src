--TEST--
Native array lookups in a container that type inference proves to be an array
--DESCRIPTION--
Reads, isset() and ?? tests and write fetches of a CV array whose inferred
type is exactly an array take the table without testing the container:
literal and runtime keys, shared arrays that a write separates, nested
writes, references to elements and missing keys still behave like the VM.
--EXTENSIONS--
native_mir_test
--FILE--
<?php
function known(int $n, string $k) {
    $a = ['x' => 1, 'y' => null, 5 => 'five', 'z' => [1, 2]];
    $out = [];
    for ($i = 0; $i < $n; $i++) {
        $out[] = isset($a['x']) ? 'x' : '-';
        $out[] = isset($a['y']) ? 'y' : '-';
        $out[] = isset($a[5]) ? '5' : '-';
        $out[] = isset($a[$k]) ? "k:$k" : "-k";
        $out[] = isset($a[$i]) ? "i:$i" : "-i";
        $out[] = $a['x'] . $i;
        $out[] = $a[5];
        $out[] = $a[$k] ?? 'dflt';
        $out[] = $a['nope'] ?? 'none';
        $out[] = $a[$i % 7] ?? 'none-i';
        $a['x'] = $i;           // write on rc=1
        $b = $a;                // share
        $a['y'] = $i * 2;       // write on rc=2 -> separate
        $out[] = $b['y'] ?? 'b-null';
        $a['z'][] = $i;         // nested write
        unset($a['zz']);
        $a[$k] = $i;
        $out[] = count($a['z']);
        $r = &$a['x'];          // reference element
        $r = 'ref';
        $out[] = $a['x'];
        unset($r);
    }
    $out[] = @$a['missing'];
    return implode(',', $out);
}
function known_int_keys() {
    $p = [10, 20, 30];
    $s = 0;
    for ($i = 0; $i < 3; $i++) { $s += $p[$i]; $p[$i] = $s; }
    return [$s, $p, isset($p[2]), isset($p[3]), $p[1] ?? 0];
}
echo known(4, 'x'), "\n", known(3, 'q'), "\n";
var_dump(known_int_keys());
echo $u = (function () { $a = []; $a['k'] = 1; echo $a['missing']; return 'done'; })(), "\n";
?>
--EXPECTF--
x,-,5,k:x,-i,10,five,1,none,none-i,b-null,3,ref,x,y,5,k:x,-i,ref1,five,ref,none,none-i,0,4,ref,x,y,5,k:x,-i,ref2,five,ref,none,none-i,2,5,ref,x,y,5,k:x,-i,ref3,five,ref,none,none-i,4,6,ref,
x,-,5,-k,-i,10,five,dflt,none,none-i,b-null,3,ref,x,y,5,k:q,-i,ref1,five,0,none,none-i,0,4,ref,x,y,5,k:q,-i,ref2,five,1,none,none-i,2,5,ref,
array(5) {
  [0]=>
  int(60)
  [1]=>
  array(3) {
    [0]=>
    int(10)
    [1]=>
    int(30)
    [2]=>
    int(60)
  }
  [2]=>
  bool(true)
  [3]=>
  bool(false)
  [4]=>
  int(30)
}

Warning: Undefined array key "missing" in %s on line 40
done
