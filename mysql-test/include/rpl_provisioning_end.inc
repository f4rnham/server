# ==== Purpose ====
#
# Shut down provisioning setup by include/rpl_provisioning_init.inc
# 
# Clears debug flag, re-creates test database and executes include/rpl_end.inc
#
# ==== Usage ====
#
# [--let $rpl_only_running_threads= 1]
# [--let $rpl_debug= 1]
#   --source include/rpl_provisioning_end.inc
#
# Parameters:
#   $rpl_debug
#     See include/rpl_init.inc

--let $include_filename= rpl_provisioning_end.inc
--source include/begin_include_file.inc

--connection master
--disable_warnings
DROP DATABASE IF EXISTS test;
--enable_warnings
CREATE DATABASE test;
SET GLOBAL debug_dbug="";

--connection slave
SET GLOBAL debug_dbug="";
--disable_warnings
DROP DATABASE IF EXISTS test;
--enable_warnings
CREATE DATABASE test;
RESET SLAVE;

--let $rpl_only_running_threads= 1
--source include/rpl_end.inc

if ($use_checksums)
{
  --connection master
  set @@global.binlog_checksum = @save_binlog_checksum;
}

--let $include_filename= rpl_provisioning_end.inc
--source include/end_include_file.inc
