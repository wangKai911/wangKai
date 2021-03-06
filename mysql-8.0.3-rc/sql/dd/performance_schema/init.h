/* Copyright (c) 2014, 2017 Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA */

#ifndef DD__PERFORMANCE_SCHEMA_INIT_PFS_INCLUDED
#define DD__PERFORMANCE_SCHEMA_INIT_PFS_INCLUDED

namespace dd {
class Properties;
enum class enum_dd_init_type;
}

namespace dd {
namespace performance_schema {

/**
  Creates performance schema tables in the Data Dictionary.

  @return       Return true upon failure, otherwise false.
*/

bool init_pfs_tables(enum_dd_init_type int_type);


/**
  Store the property "PS_version" in table's options
*/

void set_PS_version_for_table(dd::Properties *table_options);

}
}

#endif
