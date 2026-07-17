<?php
fwrite(STDOUT, "prefix\0suffix\xff");
fwrite(STDERR, "error\0bytes");
