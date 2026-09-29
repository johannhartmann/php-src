--TEST--
Native property reads into CVs and register operands of fused comparisons
--DESCRIPTION--
OPcache folds `$cv = $this->prop` into a FETCH_OBJ_R with a CV result, which
must write the slot that later reads use. A property read feeding a fused
comparison is published to its slot, so the comparison neither reads a stale
temporary nor releases it a second time.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class Parser {
    private $html;
    private $at = 0;
    public $state = 0;

    public function __construct($html) { $this->html = $html; }

    public function next($limit) {
        $was_at = $this->at;
        $at = $was_at;
        while ($at < $limit) {
            if ($at > $was_at) {
                return $at;
            }
            ++$at;
        }
        return -1;
    }

    public function skip() {
        $length = strlen($this->html);
        $skipped = strspn($this->html, " \t/", $this->at);
        $this->at += $skipped;
        if ($this->at >= $length) {
            $this->state = 1;
            return false;
        }
        return true;
    }
}

for ($i = 0; $i < 3; $i++) {
    $html = '<li class="a">' . $i;
    $parser = new Parser($html);
    var_dump($parser->next(9), $parser->next(9));
    $parser->skip();
    $parser->skip();
    $parser->skip();
    $parser = null;
    $html .= '!';
    echo $html, "\n";
}
?>
--EXPECT--
int(1)
int(1)
<li class="a">0!
int(1)
int(1)
<li class="a">1!
int(1)
int(1)
<li class="a">2!
