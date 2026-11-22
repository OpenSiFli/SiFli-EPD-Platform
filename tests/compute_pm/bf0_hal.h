#ifndef COMPUTE_PM_TEST_HAL_H
#define COMPUTE_PM_TEST_HAL_H
#include <stdbool.h>
#define RCC_MOD_ACPU 4
bool HAL_RCC_IsModuleEnabled(int module);
void HAL_RCC_DisableModule(int module);
void HAL_RCC_EnableModule(int module);
void test_dsb(void);
#define __DSB() test_dsb()
#endif
