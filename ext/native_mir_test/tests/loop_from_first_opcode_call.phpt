--TEST--
Native: A call at the head of a loop that starts at the first opcode runs its setup every iteration
--DESCRIPTION--
A loop back to opcode 0 gets an entry block of its own whose constants
name source position 0. The call's INIT lands at the loop header, not in
that entry block, so every iteration sets up the call it then runs.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class NativeTokenizer {
    public $carryOn = true;
    public $n = 0;
    public function parse() { do { $this->consume(); } while ($this->carryOn); }
    public function parseFor() { for (;;) { $this->consume(); if (!$this->carryOn) { return $this->n; } } }
    protected function consume() { $x = strlen(str_repeat('a', ++$this->n)); if ($this->n % 5 === 0) { $this->carryOn = false; } return $x; }
}
function loop_at_start(NativeTokenizer $t) { do { $t->carryOn = $t->n < 12; $t->n++; } while ($t->carryOn); return $t->n; }
$t = new NativeTokenizer; $t->parse(); var_dump($t->n);
$t->carryOn = true; var_dump($t->parseFor());
var_dump(loop_at_start(new NativeTokenizer));
--EXPECT--
int(5)
int(10)
int(13)
