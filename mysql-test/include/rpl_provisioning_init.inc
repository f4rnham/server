# ==== Purpose ====
#
# Configure two servers to be replication master and slave
# for provisioning purposes.  
#
# ==== Usage ====
#
#   [--let $rpl_server_count= N]
#   [--let $rpl_check_server_ids= 1]
#   [--let $rpl_skip_reset_master_and_slave= 1]
#   [--let $rpl_skip_change_master= 1]
#   [--let $rpl_skip_start_slave= 1]
#   [--let $rpl_debug= 1]
#   [--let $slave_timeout= NUMBER]
#   --source include/rpl_provisioning_inite.inc
#
# Parameters:
#   $rpl_check_server_ids, $rpl_skip_reset_master_and_slave,
#   $rpl_skip_change_master, $rpl_skip_start_slave, $rpl_debug,
#   $slave_timeout
#     See include/rpl_init.inc
#
#   $rpl_server_count
#     See include/master-slave.inc

--let $include_filename= rpl_provisioning_init.inc
--source include/begin_include_file.inc

# Replication requires row binlog events
--source include/have_binlog_format_mixed_or_row.inc

# Start regular replication
--source include/master-slave.inc

# Enable debug code required for testing, for example ignore 'mtr' database
--connection slave
SET GLOBAL debug_dbug="+d,provisioning_test_running";
--connection master
SET GLOBAL debug_dbug="+d,provisioning_test_running";

# Stop and reset slave before provisioning
--connection slave
--source include/stop_slave.inc
RESET SLAVE;
CHANGE MASTER TO MASTER_USE_GTID= slave_pos;
DROP DATABASE test;

--let $include_filename= rpl_provisioning_init.inc
--source include/end_include_file.inc
