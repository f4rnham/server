# ==== Purpose ====
#
# Serves as replacement for LOAD DATA FROM MASTER command on provisioning
# setup initialized by include/rpl_provisioning_init.inc
# 
# Sets 'provisioning_wait' debug flag on 'master' connection followed by
# LOAD DATA FROM MASTER command on 'slave' connection. Then waits until
# IO threads reaches "Waiting for master to send event" state or until
# a timeout is reached. Then it removes 'provisioning_wait' debug flag.
#
# Purpose of this sequence is to get to state where COM_BINLOG_DUMP was
# already recieved by master because that point determines binlog position
# from which he starts sending new events.
# When regular LOAD DATA FROM MASTER is used in test followed by query on
# master, that query may be processed before slave IO thread even starts
# causing it to not be sent to slave during provisioning.
#
# ==== Usage ====
#
# [--let $slave_timeout= NUMBER]
# [--let $rpl_debug= 1]
# [--let $master_connection= master]
# --source include/load_data_from_master.inc
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


--let $include_filename= load_data_from_master.inc
--source include/begin_include_file.inc


if (!$rpl_debug)
{
  --disable_query_log
}

if (!$master_connection)
{
  --let $master_connection= master
}

# Save old debug flags, provisioning_wait may be already present there
--connection $master_connection
SET @old_dbug= @@GLOBAL.debug_dbug;
SET GLOBAL debug_dbug= "+d,provisioning_wait";

--connection slave
LOAD DATA FROM MASTER;
--let $slave_param= Slave_IO_State
--let $slave_param_value= Waiting for master to send event
--source include/wait_for_slave_param.inc

# Restore debug flags
--connection $master_connection
SET GLOBAL debug_dbug= @old_dbug;

--let $include_filename= load_data_from_master.inc
--source include/end_include_file.inc
