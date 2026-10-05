--TEST--
Array literals assigned under integer keys are counted without opcache
--FILE--
<?php
function literal_values(int $n) {
    $a = ['k' => 0];
    for ($i = $n; $i > 0; $i--) {
        $a[$i] = [1, 2];
        $a[$i] = [3, $i];
        $a[0] = ['x' => 'y'];
    }
    $b = $a;
    $b[1][] = 'changed';
    return [$a, $b];
}
for ($r = 0; $r < 3; $r++) {
    echo json_encode(literal_values(3)), "\n";
}
?>
--EXPECT--
[{"k":0,"3":[3,3],"0":{"x":"y"},"2":[3,2],"1":[3,1]},{"k":0,"3":[3,3],"0":{"x":"y"},"2":[3,2],"1":[3,1,"changed"]}]
[{"k":0,"3":[3,3],"0":{"x":"y"},"2":[3,2],"1":[3,1]},{"k":0,"3":[3,3],"0":{"x":"y"},"2":[3,2],"1":[3,1,"changed"]}]
[{"k":0,"3":[3,3],"0":{"x":"y"},"2":[3,2],"1":[3,1]},{"k":0,"3":[3,3],"0":{"x":"y"},"2":[3,2],"1":[3,1,"changed"]}]
