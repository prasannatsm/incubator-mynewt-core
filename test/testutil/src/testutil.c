/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 * 
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
#include <assert.h>
#include "sysinit/sysinit.h"
#include "os/os.h"
#include "hal/hal_flash.h"
#include "hal/hal_system.h"
#include "testutil/testutil.h"
#include "testutil_priv.h"

#include <errno.h>
#include <unistd.h>

struct tc_config tc_config;
struct tc_config *tc_current_config = &tc_config;

struct ts_config ts_config;
struct ts_config *ts_current_config = &ts_config;

int tu_any_failed;
int tu_first_idx;

struct ts_testsuite_list *ts_suites;

int
tu_init(void)
{
    sysinit();

    return 0;
}

void
tu_arch_restart(void)
{
#if MYNEWT_VAL(SELFTEST)
    os_arch_os_stop();
    tu_case_abort();
#else
    hal_system_reset();
#endif
}

int
tu_parse_args(int argc, char **argv)
{
    int ch;

    while ((ch = getopt(argc, argv, "s")) != -1) {
        switch (ch) {
        case 's':
            ts_config.ts_system_assert = 1;
            break;

        default:
            return EINVAL;
        }
    }

    return 0;
}

void
tu_restart(void)
{
    tu_case_write_pass_auto();

    tu_first_idx = tu_case_idx + 1;

    if (ts_config.ts_restart_cb != NULL) {
        ts_config.ts_restart_cb(ts_config.ts_restart_arg);
    }

    tu_arch_restart();
}
