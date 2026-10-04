--TEST--
Inline array probes of containers and keys held as machine values
--DESCRIPTION--
isset(), empty() and ?? of an element whose container is a fetched
property temporary or whose key is computed are answered by the inline
probe after the operand is published to its frame slot; the temporary is
released once, and missing, null and falsy elements match stock PHP.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class RCache {
    public $cache = ['core' => ['a' => 1, 'z' => null, 'e' => ''], 'counts' => []];
    public $list = [10, 20];
    function has($group, $key) {
        return isset($this->cache[$group]) && isset($this->cache[$group][$key]);
    }
    function blank($group, $key) { return empty($this->cache[$group][$key]); }
    function get($group, $key) { return $this->cache[$group][$key] ?? 'default'; }
    function pick($prefix, $name, $n) {
        $k = $prefix . $name;
        return [isset($this->cache['core'][$k]), $this->cache['core'][$k] ?? '-',
            isset($this->list[$n - 1]), $this->list[$n] ?? 'none'];
    }
}
$c = new RCache;
for ($i = 0; $i < 3; $i++) {
    echo json_encode([$c->has('core', 'a'), $c->has('core', 'z'), $c->has('counts', 'a'),
        $c->has('missing', 'a'), $c->blank('core', 'e'), $c->blank('core', 'a'),
        $c->get('core', 'a'), $c->get('core', 'z'), $c->get('nope', 'x'),
        $c->pick('', 'a', 1), $c->pick('', 'q', 2)]), "\n";
}
$copy = $c->cache;
$c->cache['core']['a'] = 2;
echo $copy['core']['a'], ' ', $c->get('core', 'a'), "\n";
?>
--EXPECT--
[true,false,false,false,true,false,1,"default","default",[true,1,true,20],[false,"-",true,"none"]]
[true,false,false,false,true,false,1,"default","default",[true,1,true,20],[false,"-",true,"none"]]
[true,false,false,false,true,false,1,"default","default",[true,1,true,20],[false,"-",true,"none"]]
1 2
