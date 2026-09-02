// SPDX-License-Identifier: GPL-2.0-only
//
//  Host test runner. Exit code == number of failed tests (0 == all pass).

#include <stdio.h>

int g_tests_run = 0;
int g_tests_failed = 0;
int g_checks_failed_in_test = 0;

void run_protocol_defs_suite(void);
void run_host_comm_suite(void);
void run_panel_policy_suite(void);

int main(void) {
    printf("=== front-panel host tests ===\n");

    run_protocol_defs_suite();
    run_host_comm_suite();
    run_panel_policy_suite();

    printf("------------------------------\n");
    printf("%d tests run, %d failed\n", g_tests_run, g_tests_failed);

    return g_tests_failed;
}
