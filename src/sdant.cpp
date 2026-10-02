/*
(Modified) MIT License

Copyright (c) 2020 Gábor Ziegler and other contributors

Portions of this repo contains sourcecode either inspired by or copied from 
published code from Adafruit Industires, from thisisant.com and from 
Nordic Semiconductor ASA. The main inputs were:
* Adafruit_nRF52_Arduino repo and various public forks of that (LGPL License)
* The nRF5 SDK by Nordic Semiconductor (a mashup of licenses)
* Various ANT+ software from thisisant.com

The license conditions of particular files can be found in the top of the 
individual files. The TL/DR summary of the restrictions beyond the usual 
 MIT license:
* This software, with or without modification, must only be used with a
  Nordic Semiconductor ASA integrated circuit.
* The user if this software, with or without modification, must comply with
  the ANT licensing terms: https://www.thisisant.com/developer/ant/licensing.
  (Note particluarly that the said ANT license permits only non-commercial, 
  non revenue-generating usage without paying a yearly license fee.)

The rest of this library, which are original contributions or
derivative works falls under the MIT license. 

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software. The notifications about the 
legal requirements of adhering to the Nordic Semiconductor ASA and the
thisiant.com licensing terms shall be included.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "sdant.h"

// 注意：不要在单个 .cpp 里 `#define ANT_LICENSE_KEY`。
// bluefruit.h 用 #ifdef ANT_LICENSE_KEY 决定是否声明 setMultiprotocolSemaphore()
// 以及成员 _mprot_event_sem；如果只有本文件定义，本 TU 与 bluefruit.cpp 看到的
// 类布局会不一致（ODR 违规），调用时会写坏相邻成员。
// 该宏必须由全局编译选项 -DANT_LICENSE_KEY=... 提供，且仅在使用
// ANT 版 Adafruit nRF52 BSP（sd_softdevice_enable 带 license key 参数）时启用。

#ifndef CFG_ANT_TASK_STACKSIZE
#define CFG_ANT_TASK_STACKSIZE (256 * 5)
#endif

// ANT 事件队列的轮询周期（毫秒）。
// 原实现依赖 Bluefruit 的 setMultiprotocolSemaphore() 唤醒任务，而该 API 只在
// 定义了 ANT_LICENSE_KEY 的 ANT 版 BSP 中才存在并会被 give；在标准 BSP 下没人
// give 这个信号量，任务会 portMAX_DELAY 永久阻塞、ANT 事件永远不被处理（表现为
// 通道看起来已打开但空口没有任何数据）。这里改为“可被唤醒，否则按周期轮询”，
// 于是无论 BSP 是否提供唤醒源都能稳定取事件。
// sd_ant_event_get() 在队列为空时返回 NRF_ERROR_NOT_FOUND，因此轮询是安全的。
#ifndef CFG_ANT_EVENT_POLL_MS
#define CFG_ANT_EVENT_POLL_MS 2
#endif

SdAnt ANTplus;

void adafruit_ant_task(void *arg);

#if CFG_DEBUG
static void nrf_error_cb(uint32_t id, uint32_t pc, uint32_t info)
{
  PRINT_INT(id);
  PRINT_HEX(pc);
  PRINT_HEX(info);

  if (id == NRF_FAULT_ID_SD_ASSERT && info != 0)
  {
    typedef struct
    {
      uint16_t line_num;          /**< The line number where the error occurred. */
      uint8_t const *p_file_name; /**< The file in which the error occurred. */
    } assert_info_t;

    assert_info_t *assert_info = (assert_info_t *)info;

    LOG_LV1("SD Err", "assert at %s : %d", assert_info->p_file_name, assert_info->line_num);
  }

  while (1)
    yield();
}
#endif

// Constructor
SdAnt::SdAnt(void)
{
  _ant_event_sem = NULL;
  _ant_event_cb = NULL;
}

bool SdAnt::begin(uint8_t ant_count)
{
  if (ant_count <= 0)
    return false; //no channels needed

  // ###################################################################
  // sd_softdevice_enable is supposed to be called already by Bluefruit.
  // If not then we  return with false
  // ####################################################################
  //check if SD is enabled
  uint8_t sd_enabled = 0;
  sd_softdevice_is_enabled(&sd_enabled);
  if (sd_enabled != 1)
  {
    Serial.println("[ANT] SoftDevice 未启用，ANT 无法初始化（应先调用 Bluefruit.begin()）。");
    return false; //SD must be enabled!
  }

  // sd_softdevice_enable(&clock_cfg, nrf_error_cb, ANT_LICENSE_KEY), false );

  m_ant_stack_buffer = (uint8_t *)malloc(ANT_ENABLE_GET_REQUIRED_SPACE(ant_count, 0, 128));
  if (m_ant_stack_buffer == NULL)
  {
    Serial.println("[ANT] ANT 协议栈内存分配失败。");
    return false;
  }

  ANT_ENABLE ant_enable_cfg =
      {
          .ucTotalNumberOfChannels = ant_count,
          .ucNumberOfEncryptedChannels = 0,
          .pucMemoryBlockStartLocation = m_ant_stack_buffer,
          .usMemoryBlockByteSize = (uint16_t)(ANT_ENABLE_GET_REQUIRED_SPACE(ant_count, 0, 128))
      };

  uint32_t ant_err = sd_ant_enable(&ant_enable_cfg);
  if (ant_err != NRF_SUCCESS)
  {
    Serial.printf("[ANT] sd_ant_enable 失败: 0x%08X\n", (unsigned int)ant_err);
    Serial.println("[ANT] 情况一：当前烧录的 SoftDevice 不含 ANT 协议栈（BLE-only S140）。");
    Serial.println("[ANT] 情况二：已烧 ANT 版 SD，但 ANT 许可密钥未在 sd_softdevice_enable 时传入");
    Serial.println("[ANT]         （标准 Adafruit BSP 用的是两参数版本，无法传密钥）。");
    Serial.println("[ANT] 两种情况都需要 ANT 版 BSP/SoftDevice，否则 ANT+ 通道无法建立，码表搜不到本设备。");
    free(m_ant_stack_buffer);
    m_ant_stack_buffer = NULL;
    return false;
  }

  memset(m_ant_plus_network_key, 0, 8);
  uint8_t ant_plus_network_key[] = {0xB9, 0xA5, 0x21, 0xFB, 0xBD, 0x72, 0xC3, 0x45};
  memcpy(m_ant_plus_network_key, ant_plus_network_key, 8);

  uint32_t key_err = sd_ant_network_address_set(0, m_ant_plus_network_key);
  if (key_err != NRF_SUCCESS)
  {
    Serial.printf("[ANT] 设置 ANT+ 网络密钥失败: 0x%08X\n", (unsigned int)key_err);
    return false;
  }

  // memset(m_ant_fs_network_key, 0, 8);
  // #ifdef ANT_FS_NETWORK_KEY
  //    uint8_t ant_fs_network_key[] = ANT_FS_NETWORK_KEY;
  //    memcpy(m_ant_fs_network_key, ant_fs_network_key, 8);
  //    sd_ant_network_address_set(0, m_ant_fs_network_key);
  // #endif

  uint8_t channel = 0;
  // do setup of registered profiles
  // 原实现丢弃了 Setup() 的返回值：一旦通道分配/参数设置/打开失败，串口毫无提示，
  // 但空口上什么都不会发。这里把失败原因打出来，便于判断 ANT+ 是否真的起来了。
  for (ANTProfileEntry *entry = m_profile_list.m_head; entry != NULL; entry = entry->m_next)
  {
    uint32_t setup_err = entry->m_entry->Setup(channel);
    if (setup_err != NRF_SUCCESS)
    {
      Serial.printf("[ANT] 通道 %u (%s) 配置/打开失败: 0x%08X\n",
                    (unsigned int)channel,
                    entry->m_entry->getName(),
                    (unsigned int)setup_err);
    }
    else
    {
      Serial.printf("[ANT] 通道 %u (%s) 已打开，开始广播\n",
                    (unsigned int)channel,
                    entry->m_entry->getName());
    }
    channel++;
  }
  // Create RTOS Semaphore & Task for ANT Event
  _ant_event_sem = xSemaphoreCreateBinary();
  if (_ant_event_sem == NULL)
  {
    Serial.println("[ANT] ANT 事件信号量创建失败。");
    return false;
  }

  TaskHandle_t ant_task_hdl;
  if (xTaskCreate(adafruit_ant_task, "ANT", CFG_ANT_TASK_STACKSIZE, NULL, TASK_PRIO_HIGH, &ant_task_hdl) != pdPASS)
  {
    Serial.println("[ANT] ANT 事件任务创建失败。");
    return false;
  }

  // 仅在使用 ANT 版 BSP（全局定义 ANT_LICENSE_KEY）时，Bluefruit 的 SD 事件
  // 处理函数才会 give 这个信号量；否则该成员不存在，调用会破坏对象内存布局。
  // 注意：这里只是“快速唤醒”优化，adafruit_ant_task() 现在带超时轮询，
  // 即使没有这一句也能正常取到 ANT 事件。
#ifdef ANT_LICENSE_KEY
  Bluefruit.setMultiprotocolSemaphore(_ant_event_sem);
#endif

  return true;
}

void SdAnt::setANTEventCallback(void (*fp)(ant_evt_t *))
{
  _ant_event_cb = fp;
}

/*------------------------------------------------------------------*/
/* ANT Event handler
 *------------------------------------------------------------------*/
void adafruit_ant_task(void *arg)
{
  (void)arg;

  // malloc buffered is algined by 4
  ant_evt_t *ant_evt = (ant_evt_t *)rtos_malloc(sizeof(ant_evt_t));
  if (ant_evt == NULL)
  {
    vTaskDelete(NULL);
    return;
  }

  while (1)
  {
    // 优先由 SoftDevice 事件唤醒（ANT 版 BSP 会 give 该信号量）；
    // 若当前 BSP 没有任何地方 give 它，则超时后自行轮询。
    // 两条路径都会继续往下“排空事件队列”，因此 ANT 事件不会因缺少唤醒源而积压，
    // 也就不会出现“通道已打开但一个 EVENT_TX 都取不到”的静默失效。
    xSemaphoreTake(ANTplus._ant_event_sem, pdMS_TO_TICKS(CFG_ANT_EVENT_POLL_MS));

    uint32_t ret = NRF_SUCCESS;
    while (ret == NRF_SUCCESS)
    {
      ret = sd_ant_event_get(&ant_evt->channel, &ant_evt->event, ant_evt->message.aucMessage);
      if (ret == NRF_SUCCESS)
        ANTplus._ant_handler(ant_evt);
      // NRF_ERROR_NOT_FOUND 表示队列已空，退出内层循环等待下一轮
    }
  }
}

/**
 * ANT event handler
 * @param evt event
 */
void SdAnt::_ant_handler(ant_evt_t *evt)
{
  for (ANTProfileEntry *entry = m_profile_list.m_head; entry != NULL; entry = entry->m_next)
  {
    entry->m_entry->ProcessMessage(evt);
  }
}

void SdAnt::AddProfile(ANTProfile *p)
{
  m_profile_list.AddProfile(p);
}
