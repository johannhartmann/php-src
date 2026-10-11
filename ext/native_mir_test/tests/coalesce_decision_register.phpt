--TEST--
Native ?? keeps the helper's decision while its result is loaded
--DESCRIPTION--
$params = $params ?? $this->m(...) in a method whose callee-saved
registers are taken: the coalesce helper returns its decision in RAX,
the branch loads the array result into a register, then tests the
decision. The decision's register must stay reserved across that load
(the helper's exception check releases it at compile time); a result
loaded into RAX made the branch take the defined-value edge and left
$params undefined.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
/* A coalesce whose array result the generic helper decides, in a method
 * whose callee-saved registers are taken: the result load must not take
 * the register that holds the helper's decision. */
class NativeCoalesceClient
{
    public function send(array $params, string|array|null $stdin, int $limit, int $delay): string
    {
        return count($params) . ':' . implode(',', $params) . ':' . var_export($stdin, true)
            . ":$limit:$delay";
    }
}

class NativeCoalesceTester
{
    public $response;
    public bool $debug = false;
    private array $log = [];

    private function getRequestParams(string $query, array $headers, ?string $uri,
        ?string $scriptFilename, ?string $scriptName, string|array|null $stdin, ?string $method): array
    {
        return ['q' => $query, 'u' => $uri ?? '-', 'f' => $scriptFilename ?? '-', 'n' => $scriptName ?? '-',
            'm' => $method ?? 'GET', 'h' => count($headers)];
    }

    private function trace(string $what, $data): void
    {
        if ($this->debug) {
            echo $what, "\n";
        }
    }

    private function getClient(?string $address, bool $connKeepAlive, bool $socketKeepAlive): NativeCoalesceClient
    {
        return new NativeCoalesceClient();
    }

    private function createResponse($data = null, bool $expectInvalid = false): string
    {
        return (string) $data;
    }

    private function message(?string $message): void
    {
        $this->log[] = $message ?? 'ok';
    }

    public function request(
        string $query = '',
        array $headers = [],
        ?string $uri = null,
        ?string $address = null,
        ?string $successMessage = null,
        ?string $errorMessage = null,
        bool $connKeepAlive = false,
        bool $socketKeepAlive = false,
        ?string $scriptFilename = null,
        ?string $scriptName = null,
        string|array|null $stdin = null,
        bool $expectError = false,
        int $readLimit = -1,
        int $writeDelay = 0,
        ?string $method = null,
        ?array $params = null,
    ): string {
        if (is_array($stdin)) {
            $stdin = implode('&', $stdin);
        }

        $params = $params ?? $this->getRequestParams($query, $headers, $uri, $scriptFilename, $scriptName, $stdin, $method);
        $this->trace('Request params', $params);

        try {
            $this->response = $this->createResponse(
                $this->getClient($address, $connKeepAlive, $socketKeepAlive)
                    ->send($params, $stdin, $readLimit, $writeDelay)
            );
            if ($expectError) {
                $this->message('unexpected');
            } else {
                $this->message($successMessage);
            }
        } catch (\Exception $exception) {
            $this->message($errorMessage);
            $this->response = $this->createResponse();
        }

        return $this->response;
    }
}

$tester = new NativeCoalesceTester();
for ($i = 0; $i < 3; $i++) {
    echo $tester->request('a=' . $i), "\n";
    echo $tester->request('b', ['X' => 1], '/u', null, 'done', null, false, false, 'f.php', 'n',
        ['s', 't'], false, 10, 2, 'POST'), "\n";
    echo $tester->request(params: ['given' => 'yes', 'i' => $i]), "\n";
}
--EXPECT--
6:a=0,-,-,-,GET,0:NULL:-1:0
6:b,/u,f.php,n,POST,1:'s&t':10:2
2:yes,0:NULL:-1:0
6:a=1,-,-,-,GET,0:NULL:-1:0
6:b,/u,f.php,n,POST,1:'s&t':10:2
2:yes,1:NULL:-1:0
6:a=2,-,-,-,GET,0:NULL:-1:0
6:b,/u,f.php,n,POST,1:'s&t':10:2
2:yes,2:NULL:-1:0
