--TEST--
Native materialization does not use a later guarded loop mutation
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$source = <<<'PHP'
<?php
function guarded_phi_create_file($filename, $mode = "w") {
    $file_handle = fopen($filename, $mode);
    if ($file_handle == false) {
        return false;
    }
    fclose($file_handle);
    return true;
}

function guarded_phi_fill_file($file_handle, $fill_type, $file_size) {
    if ($fill_type == "empty") {
        return true;
    }
    if ($fill_type == "text") {
        $data = "text ";
        $size_divider = strlen($data);
        $add_value = strlen($data);
    } else if ($fill_type == "text_with_new_line") {
        $data = "line\nline of text\n";
        $size_divider = strlen($data);
        $add_value = strlen($data);
    } else if ($fill_type == "alphanumeric") {
        $data = "ab12 ";
        $size_divider = strlen($data);
        $add_value = strlen($data);
    } else if ($fill_type == "numeric") {
        $data = 2;
        $size_divider = 1;
        $add_value = 0;
    } else {
        return false;
    }

    $size = $file_size;
    $chunk_size = 1024;
    if ($size > $chunk_size) {
        do {
            if ($size <= $chunk_size) {
                $chunk_size = $size;
            }
            $num_values = str_repeat(
                $data,
                (int) (($chunk_size / $size_divider) + $add_value),
            );
            $bytes_written = fwrite($file_handle, $num_values, $chunk_size);
            if ($bytes_written != $chunk_size) {
                return false;
            }
            $size -= $chunk_size;
        } while ($size > 0);
    } else {
        $num_values = str_repeat(
            $data,
            (int) (($chunk_size / $size_divider) + $add_value),
        );
        $bytes_written = fwrite($file_handle, $num_values, $file_size);
        if ($bytes_written != $file_size) {
            return false;
        }
    }
    return true;
}

function guarded_phi_change_file_perms(
    $file_path,
    $count = 1,
    $perms = 0755,
    $name_prefix = "file",
    $name_suffix = 1,
    $file_extension = ".tmp",
) {
    $changed = 0;
    if ($count <= 0) {
        return $changed;
    }
    if ($name_suffix <= 0) {
        $name_suffix = 1;
    }
    for ($loop_counter = 1; $loop_counter <= $count; $loop_counter++) {
        $filename = $file_path . "/" . $name_prefix . $name_suffix . $file_extension;
        if (chmod($filename, $perms)) {
            $changed++;
        }
        $name_suffix++;
    }
    return $changed;
}

function guarded_mutation_phi_root(
    $file_path,
    $count = 1,
    $content_type = "numeric",
    $permission = 0755,
    $size = 1,
    $mode = "w",
    $name_prefix = "file",
    $name_suffix = 1,
    $flag = "kilobytes",
    $file_extension = ".tmp",
) {
    $return_value = array('created' => 0, 'filled' => 0, 'perms_changed' => 0);
    if ($name_suffix <= 0) {
        $name_suffix = 1;
    }
    if ($size == 0) {
        return $return_value;
    }
    $file_size = $size;
    if ($flag == "kilobytes") {
        $file_size = $file_size * 1024;
    }

    $tmp_name_suffix = $name_suffix;
    for ($file_created_count = 1; $file_created_count <= $count; $file_created_count++) {
        $filename = $file_path . "/" . $name_prefix . $tmp_name_suffix . $file_extension;
        $status = guarded_phi_create_file($filename, $mode);
        $tmp_name_suffix++;
        if ($status == true) {
            $return_value['created']++;
        } else {
            return $return_value;
        }
    }

    if ($content_type == "empty") {
        $return_value['filled'] = $count;
    } else {
        $tmp_name_suffix = $name_suffix;
        for ($loop_counter = 1; $loop_counter <= $count; $loop_counter++) {
            $filename = $file_path . "/" . $name_prefix . $tmp_name_suffix . $file_extension;
            $file_handle = fopen($filename, $mode);
            if ($file_handle == false) {
                fclose($file_handle);
                return $return_value;
            }
            if (guarded_phi_fill_file($file_handle, $content_type, $file_size)) {
                $return_value['filled']++;
            }
            fclose($file_handle);
            $tmp_name_suffix++;
        }
    }

    $return_value['perms_changed'] = guarded_phi_change_file_perms(
        $file_path,
        $count,
        $permission,
        $name_prefix,
        $name_suffix,
        $file_extension,
    );
    return $return_value;
}
PHP;

$directory = __DIR__ . '/w11p_guarded_mutation_phi_materialization.tmp';
@mkdir($directory);
$result = native_mir_test_compile_execute(
    $source,
    'w11p-guarded-mutation-phi-materialization.php',
    [$directory, 1, 'text_with_new_line', 0755, 20, 'w', 'entry', 1, 'bytes'],
    [
        'wave' => 11,
        'function' => 'guarded_mutation_phi_root',
        'repeat' => 10,
    ],
);
@unlink($directory . '/entry1.tmp');
@rmdir($directory);

printf(
    "%s return=%s runs=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    $result['execution']['executions'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--CLEAN--
<?php
$directory = __DIR__ . '/w11p_guarded_mutation_phi_materialization.tmp';
@unlink($directory . '/entry1.tmp');
@rmdir($directory);
?>
--EXPECT--
accepted return={"created":1,"filled":1,"perms_changed":1} runs=10 vm=0 execute_ex=0 handler=0 active=0
