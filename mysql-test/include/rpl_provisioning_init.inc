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
#   [--let $invert_binlog_format= 1]
#   --source include/rpl_provisioning_init.inc
#
# Parameters:
#   $rpl_check_server_ids, $rpl_skip_reset_master_and_slave,
#   $rpl_skip_change_master, $rpl_skip_start_slave, $rpl_debug,
#   $slave_timeout
#     See include/rpl_init.inc
#
#   $rpl_server_count
#     See include/master-slave.inc
#
#   $invert_binlog_format
#     Test will be executed only in non-row binlog formats (stmt, mix)
#     opposed to usual - run test only in row binlog format

--let $include_filename= rpl_provisioning_init.inc
--source include/begin_include_file.inc

# Provisioning requires row binlog events
if (!$invert_binlog_format)
{
    --source include/have_binlog_format_row.inc
}

if ($invert_binlog_format)
{
    --source include/have_binlog_format_mixed_or_statement.inc
}

# Start regular replication
--let $rpl_skip_start_slave= 1
--source include/master-slave.inc

# Enable debug code required for testing, for example ignore 'mtr' database
--connection slave
SET GLOBAL debug_dbug="+d,provisioning_test_running";
--connection master
SET GLOBAL debug_dbug="+d,provisioning_test_running";

# Prepare slave for provisioning
--connection slave
DROP DATABASE test;

--let $include_filename= rpl_provisioning_init.inc
--source include/end_include_file.inc
