#pragma once

#include "esp_err.h"
#include <stdbool.h>

void osdp_pd_task_start(void);
bool osdp_pd_is_online(void);
void osdp_pd_load_config(int *address, int *baud);
esp_err_t osdp_pd_save_config(int address, int baud);
