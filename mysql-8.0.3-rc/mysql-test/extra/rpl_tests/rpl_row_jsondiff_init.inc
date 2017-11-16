# ==== Overview ====
#
# This script works in conjuction with:
#
#   extra/rpl_tests/rpl_row_jsondiff_[basic|datatypes|...].inc
#   extra/rpl_tests/rpl_row_jsondiff_scenario.inc
#   extra/rpl_tests/rpl_row_jsondiff_exit.inc
#
# A .test file in the suite typically does the following:
#
#   --source extra/rpl_tests/rpl_row_jsondiff_init.inc
#   --source extra/rpl_tests/rpl_row_jsondiff_[basic|datatypes|...]inc
#   --source extra/rpl_tests/rpl_row_jsondiff_exit.inc
#
# where:
# - This 'init' script sets up replication topologies and some configuration.
# - Each 'basic'/'datatypes'/etc script defines a number of scenarios.
# - A scenario is executed by invoking the 'scenario' script with appropriate
#   parameters.
# - The 'exit' script cleans up.
#
# ==== Requirements ====
#
# This script sets up a replication topology, in order to cover the
# following configurations and methods to apply rows:
#
# - Re-applying partial JSON updates should work in the following
#   cases:
#   - Replication to slave
#   - Chained replication to slave-of-slave
#   - Applying BINLOG 'base64' statements from mysqlbinlog
#   - Applying the decoded pseudo-SQL from mysqlbinlog -v, if
#     re-formatted into correct SQL.
#
# - Replication should work with minimal/full row images on
#   master/slave, including the following cases:
#   - minimal->minimal
#   - minimal->full
#   - full->full
#   - full->minimal,
#   - full->minimal->minimal.  Note: due to the semantics of
#     binlog_row_image=minimal, the first slave in this configuration
#     does not produce the same after-image as a master having
#     minimal.  When the master has full, the slave will include the
#     full after-image even when binlog_row_image=minimal, because it
#     always includes all columns that were specified.  Therefore, we
#     also test that replication to the second slave works, to verify
#     that this 'minimal with full after-image' format is correct.
#     Cf. BUG#26258329.
#
#   Since NOBLOB is similar to MINIMAL for JSON columns, we consider
#   NOBLOB tested due to having tested MINIMAL.
#
# - All the above should hold when:
#
#   - slave_rows_search_algorithms=HASH_SCAN/TABLE_SCAN with and
#     without a primary key
#
#   - slave_rows_search_algorithms=INDEX_SCAN with a primary key.
#
# ==== Usage ====
#
# [--let $replace_combination_from= SCOPE.VARIABLE=VALUE[,VALUE[,...]]
# [--let $replace_combination_to= SCOPE.VARIABLE=VALUE[,VALUE[,...]]
# --source include/rpl_row_jsondiff_init.inc
#
# [set $variables to describe a scenario]
# --source rpl_row_jsondiff_scenario.inc
#
# [set more $variables to describe another scenario]
# --source rpl_row_jsondiff_scenario.inc
#
# ...
#
# --source include/rpl_row_jsondiff_exit.inc
#
# Parameters:
#   $replace_combination_from, $replace_combination_to
#     If set, this script will source
#     include/begin_replace_combination.inc just after
#     include/rpl_init.inc, and rpl_row_jsondiff_exit.inc will source
#     include/end_replace_combination just before include/rpl_end.inc.
#     See include/begin_replace_combination.inc for details.
#
# ==== Implementation ====
#
# Setup the following replication topology:
#
#   master -> slave_1 -> slave_2 -> slave_3 -> slave_4 -> slave_5
#   decoded
#   base64
#
# where:
# - master, slave_1, slave_4, slave_5 use binlog_row_image=MINIMAL
# - slave_2, slave_3 use binlog_row_image=FULL
# - row events decoded into SQL by mysqlbinlog will be applied on 'decoded'
# - BINLOG 'base64' statements generated by mysqlbinlog will be applied on
#   'base64'
#
# Set binlog_row_value_options=PARTIAL_JSON on all servers.
#
# ==== References ====
#
# WL#2955: RBR replication of partial JSON updates


# ######## CONFIGURE ########

--source include/have_log_bin.inc

--let $rpl_server_count= 8
--let $rpl_topology= 1->2->3->4->5->6
--source include/rpl_init.inc
--source include/rpl_default_connections.inc

if ($replace_combination_from)
{
  --let $replace_combination_on_all_servers= 1
  --source include/begin_replace_combination.inc
}


--echo ######## INITIALIZE ########

# mysqltest gets confused by dollars, and does not know how to escape them
--let $dollar_func= CHAR(36)
--let $dollar= `SELECT $dollar_func`
--let $master_con= server_1
--let $slave_1_con= server_2
--let $slave_2_con= server_3
--let $slave_3_con= server_4
--let $slave_4_con= server_5
--let $slave_5_con= server_6
--let $base64_con= server_7
--let $decoded_con= server_8

--connection $master_con

# Enable PARTIAL_JSON on all servers, set binlog_row_image=MINIMAL on
# all servers except server_2 and server_3.
--disable_query_log
--delimiter |
let $rpl_sql=
  SET @@SESSION.BINLOG_ROW_IMAGE = MINIMAL;
  SET @@SESSION.BINLOG_ROW_VALUE_OPTIONS = PARTIAL_JSON;
  SET @@GLOBAL.BINLOG_ROW_IMAGE = MINIMAL;
  SET @@GLOBAL.BINLOG_ROW_VALUE_OPTIONS = PARTIAL_JSON;
|
--delimiter ;
--source include/rpl_for_each_server_stmt.inc
--disable_warnings
--connection $slave_2_con
SET @@SESSION.BINLOG_ROW_IMAGE = FULL;
SET @@GLOBAL.BINLOG_ROW_IMAGE = FULL;
--connection $slave_3_con
SET @@SESSION.BINLOG_ROW_IMAGE = FULL;
SET @@GLOBAL.BINLOG_ROW_IMAGE = FULL;
--enable_warnings
--connection $master_con
--source include/rpl_stop_slaves.inc
--source include/rpl_start_slaves.inc


# Set 'constant' parameters of rpl_row_jsondiff_scenario.test
--let $echo_stmt= 1
--let $echo_decoded_rows= 1
--let $table= test.t
--let $scenario_inc= extra/rpl_tests/rpl_row_jsondiff_scenario.inc


--disable_query_log