--TEST--
Native: A guarded fast path stores the operands its cold block takes before its first check
--DESCRIPTION--
$this->map[$a][$id[$a]] reads the inner array into a register and takes the
helper for a counted string key. The cold block reloads the container from
its stack slot, so the slot must be written before the first failed check
jumps there, not by a later register eviction on the fall-through path.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class Hydrator
{
    private array $identifierMap = [];
    private array $resultPointers = [];
    private array $rootAliases = [];
    private int $resultCounter = 0;

    private function gather(array $row, array &$id, array &$nonempty): array
    {
        $data = [];
        foreach ($row as $alias => $value) {
            $id[$alias] .= '|' . $value;
            $nonempty[$alias] = true;
            $data[$alias] = ['v' => $value];
        }
        return $data;
    }

    public function hydrateRow(array $row, array &$result): void
    {
        $id = ['p' => '', 'a' => ''];
        $nonempty = [];
        $rowData = $this->gather($row, $id, $nonempty);
        $this->resultPointers = [];
        foreach ($rowData as $dqlAlias => $data) {
            $this->rootAliases[$dqlAlias] = true;
            $entityKey = 0;
            if (!isset($nonempty[$dqlAlias])) {
                $result[] = null;
                continue;
            }
            if (!isset($this->identifierMap[$dqlAlias][$id[$dqlAlias]])) {
                $element = (object) $data;
                $resultKey = $this->resultCounter;
                ++$this->resultCounter;
                $result[] = $element;
                $this->identifierMap[$dqlAlias][$id[$dqlAlias]] = $resultKey;
                $this->resultPointers[$dqlAlias] = $element;
            } else {
                $index = $this->identifierMap[$dqlAlias][$id[$dqlAlias]];
                $this->resultPointers[$dqlAlias] = $result[$index];
                $resultKey = $index;
            }
        }
    }
}
$h = new Hydrator; $result = [];
foreach ([[1, 1], [1, 2], [2, 1], [2, 2], [3, 1], [1, 3]] as [$p, $a]) {
    $h->hydrateRow(['p' => $p, 'a' => $a], $result);
}
echo count($result), "\n";
--EXPECT--
6
