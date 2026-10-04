--TEST--
Native fused comparisons of temporaries
--DESCRIPTION--
A comparison of array elements or other temporaries fused into its branch
publishes register-held operands to their slots and compares numbers inline;
strings, null, booleans and NaN take the helper.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function hs($n, &$ra) {
    $l = ($n >> 1) + 1; $ir = $n;
    while (1) {
        if ($l > 1) { $rra = $ra[--$l]; } else { $rra = $ra[$ir]; $ra[$ir] = $ra[1]; if (--$ir == 1) { $ra[1] = $rra; return; } }
        $i = $l; $j = $l << 1;
        while ($j <= $ir) {
            if (($j < $ir) && ($ra[$j] < $ra[$j+1])) { $j++; }
            if ($rra < $ra[$j]) { $ra[$i] = $ra[$j]; $j += ($i = $j); } else { $j = $ir + 1; }
        }
        $ra[$i] = $rra;
    }
}
mt_srand(3); $a = []; for ($i = 1; $i <= 500; $i++) { $a[$i] = mt_rand(0, 1000) / 7; }
$a[7] = "12"; $a[9] = NAN; $a[11] = null; $a[13] = "abc"; $a[17] = true;
$b = $a; hs(500, $b); echo md5(serialize($b)), "\n";
function t($x, $y) { $o = []; foreach ([[$x, $y], [strlen($x) , 3], [$x . "", $y . ""]] as [$p, $q]) { $o[] = ($p . "" < $q . "") ? "lt" : "ge"; if ($p <= $q) { $o[] = "le"; } if (intval($p) == $q) { $o[] = "eq"; } } return implode(",", $o); }
echo t("5", "10"), " ", t("abc", "abd"), " ", t(3, 3.0), "\n";

?>

--EXPECT--
f0ad262e66f365082eb6857af8402a35
lt,le,lt,le,lt,le lt,le,ge,le,eq,lt,le ge,le,eq,lt,le,ge,le,eq
