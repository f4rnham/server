# ==== Purpose ====
#
# Waits until the SQL thread of the current connection has stopped
# gracefully, then validates, that master and slave log file names
# and positions are equal
#
# Note: this script will fail if the SQL thread stops with an error
# or log positions do not match
#
# This script also fails if a timeout is reached (default 300
# seconds).
#
#
# ==== Usage ====
#
# [--let $slave_timeout= NUMBER]
# [--let $rpl_debug= 1]
# [--let $master_connection= master]
# --source include/rpl_provisioning_wait_to_stop.inc
#
# Parameters:
#   $slave_timeout
#     See include/wait_for_slave_param.inc
#
#   $rpl_debug
#     See include/rpl_init.inc
#
#   $master_connection
#     Use this connection name for master instead of default 'master'


--let $include_filename= rpl_provisioning_wait_to_stop.inc
--source include/begin_include_file.inc

if (!$master_connection)
{
  --let $master_connection= master
}

--source include/wait_for_slave_sql_to_stop.inc

# Ensure that master and slave log names and positions are equal
--connection $master_connection
--let $binlog_file_master= query_get_value(SHOW MASTER STATUS, File, 1)
--let $binlog_pos_master= query_get_value(SHOW MASTER STATUS, Position, 1)
--connection slave
--let $binlog_file_slave= query_get_value(SHOW SLAVE STATUS, Master_Log_File, 1)
--let $binlog_pos_slave= query_get_value(SHOW SLAVE STATUS, Read_Master_Log_Pos, 1)

if ($binlog_file_master != $binlog_file_slave)
{
  die Master-slave binlog file names mismatch;
}

if ($binlog_pos_master != $binlog_pos_slave)
{
  die Master-slave binlog position mismatch;
}

# Ensure that master and slave gtids are equal
--connection $master_connection
--let $gtid_master= `SELECT @@gtid_current_pos`
-- connection $master_connection
--let $gtid_slave= `SELECT @@gtid_current_pos`

if ($gtid_master != $gtid_slave)
{
  die Master-slave gtid mismatch;
}

--let $include_filename= rpl_provisioning_wait_to_stop.inc
--source include/end_include_file.inc
