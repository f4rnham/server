# ==== Purpose ====
#
# Configure two servers to be replication master and slave
# for provisioning purposes. Row binlog format version.
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
#   [--let $use_checksums= 1]
#   --source include/rpl_provisioning_init_row.inc
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
#   $use_checksums
#     See include/rpl_provisioning_init.inc

--let $include_filename= rpl_provisioning_init_row.inc
--source include/begin_include_file.inc

--source include/have_binlog_format_row.inc
--source include/rpl_provisioning_init.inc

--let $include_filename= rpl_provisioning_init_row.inc
--source include/end_include_file.inc
