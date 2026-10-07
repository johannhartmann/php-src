--TEST--
Native images loaded from the OPcache file cache by another process reach globals, frameless and internal handlers through the context
--EXTENSIONS--
opcache
--FILE--
<?php
$dir = sys_get_temp_dir() . '/native_file_cache_' . getmypid();
@mkdir($dir);
@mkdir("$dir/cache");
$script = "$dir/script.php";
file_put_contents($script, <<<'PHP'
<?php
$g = 5;
function bound() { global $g; return $g + 1; }
function frameless($s, $a) { return strlen($s) . str_replace('a', 'b', $s) . (in_array($s, $a) ? 'y' : 'n'); }
function internal($a) { return count($a) . implode(',', $a) . json_encode($a); }
$t = '';
for ($i = 0; $i < 30; $i++) {
    $t .= bound() . frameless("a$i", ['a1']) . internal([$i, 'x']);
}
echo md5($t), "\n";
PHP);
$command = escapeshellarg(PHP_BINARY) . ' -n -d opcache.enable=1'
    . ' -d opcache.enable_cli=1 -d opcache.file_cache='
    . escapeshellarg("$dir/cache") . ' ' . escapeshellarg($script) . ' 2>&1';
$runs = [shell_exec($command), shell_exec($command), shell_exec($command)];
var_dump($runs[0] === $runs[1], $runs[1] === $runs[2],
    strlen(trim($runs[0])));

function remove_tree(string $path): void {
    if (is_dir($path)) {
        foreach (scandir($path) as $entry) {
            if ($entry !== '.' && $entry !== '..') {
                remove_tree("$path/$entry");
            }
        }
        rmdir($path);
    } else {
        unlink($path);
    }
}
remove_tree($dir);
?>
--EXPECT--
bool(true)
bool(true)
int(32)
