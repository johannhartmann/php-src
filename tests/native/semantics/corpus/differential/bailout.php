<?php
echo "before-bailout\n";
zend_trigger_bailout();
echo "after-bailout\n";
