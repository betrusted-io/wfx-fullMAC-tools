/***************************************************************************//**
 * @file
 * @brief WFX FMAC driver main bus communication task
 *******************************************************************************
 * # License
 * <b>Copyright 2019 Silicon Laboratories Inc. www.silabs.com</b>
 *******************************************************************************
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/
#include "sl_wfx.h"
#include "sl_wfx_registers.h"
#include "lwip_micriumos.h"
#include "sl_wfx_host_cfg.h"

#include "em_gpio.h"
#include "em_usart.h"
#include "em_cmu.h"
#include "em_ldma.h"
#include "em_bus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel/include/os.h>
#include <common/include/rtos_utils.h>
#include <common/include/rtos_err.h>
#include <common/include/rtos_err.h>

#include "sl_wfx_task.h"
#include "sl_wfx_host.h"

// Bus Task Configurations
#define WFX_BUS_TASK_PRIO              15u
#define WFX_BUS_TASK_STK_SIZE         512u
#define WFX_BUS_EVENT_TIMEOUT_MS        (0)

/// wfx bus task stack
static CPU_STK wfx_bus_task_stk[WFX_BUS_TASK_STK_SIZE];
/// wfx bus task TCB
static OS_TCB wfx_bus_task_tcb;

OS_FLAG_GRP wfx_bus_evts;
wfx_frame_q_item wfx_bus_tx_frame;
OS_SEM wfx_bus_tx_complete;

/// Flag to indicate receive frames is currently running.
static bool wfx_bus_rx_in_process = false;

/// wfx_fmac_driver context
sl_wfx_context_t wifi;

// Connection parameters
char wlan_ssid[32+1]                   = WLAN_SSID_DEFAULT;
char wlan_passkey[64+1]                = WLAN_PASSKEY_DEFAULT;
uint8_t wlan_bssid[SL_WFX_BSSID_SIZE];
sl_wfx_security_mode_t wlan_security   = WLAN_SECURITY_DEFAULT;
char softap_ssid[32+1]                 = SOFTAP_SSID_DEFAULT;
char softap_passkey[64+1]              = SOFTAP_PASSKEY_DEFAULT;
sl_wfx_security_mode_t softap_security = SOFTAP_SECURITY_DEFAULT;
uint8_t softap_channel                 = SOFTAP_CHANNEL_DEFAULT;

/***************************************************************************//**
 * Check receive frame status
 ******************************************************************************/
bool wfx_bus_is_receive_processing(void)
{
  return wfx_bus_rx_in_process;
}

/***************************************************************************//**
 * Receives frames from the WF200.
 ******************************************************************************/
static sl_status_t receive_frames()
{
  sl_status_t result;
  uint16_t control_register = 0;
  wfx_bus_rx_in_process = true;
  do {
    result = sl_wfx_receive_frame(&control_register);

    SL_WFX_ERROR_CHECK(result);
  } while ( (control_register & SL_WFX_CONT_NEXT_LEN_MASK) != 0 );
  wfx_bus_rx_in_process = false;
  error_handler:
  return result;
}

/***************************************************************************//**
 * WF200 bus communication task.
 ******************************************************************************/
static void wfx_bus_task(void *p_arg)
{
  (void)p_arg;
  RTOS_ERR err;
  sl_status_t result;
  OS_FLAGS  flags = 0;
  sl_wfx_host_setup_memory_pools();
  OSFlagCreate(&wfx_bus_evts, "wfx bus events", 0, &err);
  OSSemCreate(&wfx_bus_tx_complete, "wfx bus tx comp", 0, &err);
  for (;; ) {
#ifdef SLEEP_ENABLED
#ifdef SL_WFX_USE_SPI
    if (GPIO_PinInGet(SL_WFX_HOST_CFG_SPI_WIRQPORT, SL_WFX_HOST_CFG_SPI_WIRQPIN)) //wfx messages pending
#else
    if (GPIO_PinInGet(SL_WFX_HOST_CFG_WIRQPORT, SL_WFX_HOST_CFG_WIRQPIN))
#endif
    {
      OSFlagPost(&wfx_bus_evts, SL_WFX_BUS_EVENT_FLAG_RX, OS_OPT_POST_FLAG_SET, &err);
    }
#endif
    /*Wait for an interrupt from WFX*/
    flags = OSFlagPend(&wfx_bus_evts, 0xF, WFX_BUS_EVENT_TIMEOUT_MS,
                       OS_OPT_PEND_FLAG_SET_ANY | OS_OPT_PEND_BLOCKING | OS_OPT_PEND_FLAG_CONSUME,
                       0, &err);

    if (RTOS_ERR_CODE_GET(err) == RTOS_ERR_NONE) {

      if (flags & SL_WFX_BUS_EVENT_FLAG_RX) {
        // Treat received frames
        receive_frames();

#ifdef SL_WFX_USE_SDIO
        // Reenable interrupt (req for sdio)
        sl_wfx_host_enable_platform_interrupt();
#endif
      }
      if (flags & SL_WFX_BUS_EVENT_FLAG_TX) {
        int i = 0;
        result = SL_STATUS_FAIL;
        while ((result != SL_STATUS_OK) && (i++ < 10)) {
          result = sl_wfx_send_ethernet_frame(wfx_bus_tx_frame.frame,
                                              wfx_bus_tx_frame.data_length,
                                              wfx_bus_tx_frame.interface,
                                              wfx_bus_tx_frame.priority);
        }
        if (result != SL_STATUS_OK) {
          printf("Unable to send ethernet frame\r\n");
        }
        // Ensure to unlock the stack
        OSSemPost(&wfx_bus_tx_complete, OS_OPT_POST_ALL, &err);
      }
    } else {
      printf("Bus Task error\r\n");
    }
  }
}

/***************************************************************************//**
 * Creates WF200 bus communication task.
 ******************************************************************************/
void wfx_bus_start()
{
  RTOS_ERR err;
  OSTaskCreate(&wfx_bus_task_tcb,
               "WFX bus Task",
               wfx_bus_task,
               DEF_NULL,
               WFX_BUS_TASK_PRIO,
               &wfx_bus_task_stk[0],
               (WFX_BUS_TASK_STK_SIZE / 10u),
               WFX_BUS_TASK_STK_SIZE,
               0u,
               0u,
               DEF_NULL,
               (OS_OPT_TASK_STK_CLR),
               &err);
  // Check error code.
  APP_RTOS_ASSERT_DBG((RTOS_ERR_CODE_GET(err) == RTOS_ERR_NONE), 1);
}
