#!/bin/bash
unset DISPLAY
make test TESTOPTS="-x test.test_pydoc.test_pydoc test_cmd_line_script test_eintr test_filecmp test_import test_ntpath test_os test_pathlib test_posix test_posixpath test_shutil test_sqlite3 test_tarfile test_unicode_file test_unicode_file_functions test_zipimport test_pydoc test_peg_generator test_tkinter test_ttk test_zipfile64"
