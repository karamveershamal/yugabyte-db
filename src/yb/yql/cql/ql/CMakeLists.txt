# Copyright (c) YugaByte, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
# in compliance with the License.  You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software distributed under the License
# is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
# or implied.  See the License for the specific language governing permissions and limitations
# under the License.
#

add_subdirectory(util)
add_subdirectory(ptree)
add_subdirectory(parser)
add_subdirectory(audit)
add_subdirectory(sem)
add_subdirectory(exec)
add_subdirectory(test)

ADD_YB_LIBRARY(ql_api
               SRCS statement.cc ql_processor.cc
               DEPS ql_parser ql_audit ql_sem ql_exec ql_util)

yb_use_pch(ql_api ql)
