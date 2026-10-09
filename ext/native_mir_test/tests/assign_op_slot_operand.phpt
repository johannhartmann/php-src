--TEST--
Native: A compound assignment that reads its CV from the slot updates the slot
--DESCRIPTION--
A lazy integer mutation leaves its result in the register only when it
read the previous value from the register. $i += 2 of a loop whose value
the fast path reads from the frame slot stores the slot, or the loop
condition and the next iteration see the old value.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
$dir = __DIR__ . '/assign_op_slot_operand';
@mkdir($dir);
file_put_contents("$dir/Compiler.php", <<<'PHP'
<?php
class Compiler { public $s = ''; function write(...$x) { foreach ($x as $y) $this->s .= $y; return $this; } function raw($x) { $this->s .= $x; return $this; }
  function indent() { return $this; } function outdent() { return $this; } function subcompile(Node $n, $raw = true) { $n->compile($this); return $this; }
  function addDebugInfo(Node $n) { return $this; } }
PHP);
file_put_contents("$dir/Node.php", <<<'PHP'
<?php
class Node implements Countable {
    protected $nodes; protected $attributes;
    function __construct(array $nodes = [], array $attributes = []) { $this->nodes = $nodes; $this->attributes = $attributes; }
    function compile(Compiler $c) { foreach ($this->nodes as $n) $n->compile($c); }
    public function hasNode(string $name): bool { return isset($this->nodes[$name]); }
    public function getNode(string $name): Node { if (!isset($this->nodes[$name])) throw new LogicException("x"); return $this->nodes[$name]; }
    public function count(): int { return \count($this->nodes); }
}
PHP);
file_put_contents("$dir/TextNode.php", <<<'PHP'
<?php
class TextNode extends Node { function compile(Compiler $c) { $c->raw($this->attributes['data']); } }
PHP);
file_put_contents("$dir/IfNode.php", <<<'PHP'
<?php
class IfNode extends Node {
    public function compile(Compiler $compiler): void
    {
        $compiler->addDebugInfo($this);
        for ($i = 0, $count = \count($this->getNode('tests')); $i < $count; $i += 2) {
            if (++$GLOBALS['native_guard'] > 50) { echo "loop does not advance\n"; exit; }
            if ($i > 0) {
                $compiler
                    ->outdent()
                    ->write('} elseif (')
                ;
            } else {
                $compiler
                    ->write('if (')
                ;
            }

            $compiler
                ->subcompile($this->getNode('tests')->getNode($i))
                ->raw(") {\n")
                ->indent()
            ;
            // The node might not exists if the content is empty
            if ($this->getNode('tests')->hasNode($i + 1)) {
                $compiler->subcompile($this->getNode('tests')->getNode($i + 1));
            }
        }

        if ($this->hasNode('else')) {
            $compiler
                ->outdent()
                ->write("} else {\n")
                ->indent()
                ->subcompile($this->getNode('else'))
            ;
        }

        $compiler
            ->outdent()
            ->write("}\n");
    }
}
PHP);
spl_autoload_register(function ($c) use ($dir) { require "$dir/$c.php"; });
$GLOBALS['native_guard'] = 0;
$t = fn($d) => new TextNode([], ['data' => $d]);
$if = new IfNode(['tests' => new Node([$t('$x > 1'), $t('big'), $t('$y'), $t('mid')]), 'else' => $t('small')]);
$c = new Compiler; $if->compile($c); echo $c->s;
--EXPECT--
if ($x > 1) {
big} elseif ($y) {
mid} else {
small}
--CLEAN--
<?php
$dir = __DIR__ . '/assign_op_slot_operand';
foreach (['Compiler', 'Node', 'TextNode', 'IfNode'] as $f) { @unlink("$dir/$f.php"); }
@rmdir($dir);
