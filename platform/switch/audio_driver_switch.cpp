/*************************************************************************/
/*  audio_driver_switch.cpp                                              */
/*************************************************************************/
/*                       This file is part of:                           */
/*                           GODOT ENGINE                                */
/*                      https://godotengine.org                          */
/*************************************************************************/
/* Copyright (c) 2007-2022 Juan Linietsky, Ariel Manzur.                 */
/* Copyright (c) 2014-2022 Godot Engine contributors (cf. AUTHORS.md).   */
/*                                                                       */
/* Permission is hereby granted, free of charge, to any person obtaining */
/* a copy of this software and associated documentation files (the       */
/* "Software"), to deal in the Software without restriction, including   */
/* without limitation the rights to use, copy, modify, merge, publish,   */
/* distribute, sublicense, and/or sell copies of the Software, and to    */
/* permit persons to whom the Software is furnished to do so, subject to */
/* the following conditions:                                             */
/*                                                                       */
/* The above copyright notice and this permission notice shall be        */
/* included in all copies or substantial portions of the Software.       */
/*                                                                       */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,       */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF    */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.*/
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY  */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,  */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE     */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                */
/*************************************************************************/

// ===========================================================================
// 本文件是上游 redthing1/godot_switch（分支 3.5-stable_switch）的
// platform/switch/audio_driver_switch.cpp 的**打过补丁版本**。
//
// 改动只有两处，都用 "=== 补丁 ===" 标出，其余与上游逐字节相同。
//
// 背景（详见同目录的 README-怎么用.md）：
//   Switch 上如果音频输出是**蓝牙设备**（蓝牙音箱 / 蓝牙耳机），App 会整个卡死：
//     * 启动时已连着蓝牙 → 卡在引擎初始化，黑屏，连 Godot 启动画面都出不来
//     * 运行中连上蓝牙     → 画面冻在最后一帧
//   有线耳机没有这个问题，音频驱动设成 Dummy 也没有这个问题。
//
// 原因（在源码里可以指到行）：
//   1. init_device() 里 audrenInitialize / audrvCreate / audrvUpdate /
//      audrenStartAudioRenderer 的返回值**全都只 printf、不检查**。
//      蓝牙输出时这些调用会失败，但代码继续往下跑，带着一个不可用的渲染器。
//   2. 音频线程里有一个**没有退出条件的等待循环**：
//          while (state != Playing) { audrvUpdate(...); }
//      渲染器不可用时缓冲区永远进不了 Playing → 死循环。
//      而这个循环**持着 ad->lock()**，于是整个 App 冻住。
//
// 补丁做的事（刻意做得最小，不新增成员、不改 .h，方便整文件替换）：
//   1. 检查那几个返回值；失败就清理并返回 ERR_CANT_OPEN。
//      Godot 的 AudioDriverManager 在驱动 init 失败时会回退到 Dummy 驱动 ——
//      于是「启动时连着蓝牙」会变成「App 正常跑、暂时没声音」，而不是死机。
//   2. 把那个无限等待改成**有上限**的等待。超时就放弃这一块缓冲区继续往下走；
//      下一轮会因为找不到空闲缓冲区而走到原有的「歇 1ms」分支，不会空转烧 CPU。
//      设备恢复后音频有机会自己接着走。
//
// ⚠ 诚实说明：这个补丁**没有在真机上验证过**（我没有 Switch）。
//   它保证的是「不再死循环」，不保证「蓝牙下一定有声音」。
//   请移植版作者 review 后再发布。
// ===========================================================================

#include "audio_driver_switch.h"

#include "core/os/os.h"
#include "core/project_settings.h"

#include <errno.h>
#include <malloc.h>

static const AudioRendererConfig arConfig = {
	.output_rate = AudioRendererOutputRate_48kHz,
	.num_voices = 24,
	.num_effects = 0,
	.num_sinks = 1,
	.num_mix_objs = 1,
	.num_mix_buffers = 2,
};

// === 补丁 ===
// 等待缓冲区进入 Playing 状态的最大轮数。
// 原来这个循环是无限的；现在给它一个上限。
//
// 取值理由：正常情况下一两块缓冲区在一个音频帧内就会进入 Playing（几十微秒）。
// 2000 轮已经远超正常所需，纯粹是「渲染器坏了」和「还没轮到」的分界线。
// 调大只会让坏掉的时候多转几毫秒，调小则有可能误判。
#define SWITCH_AUDIO_WAIT_TRIES 2000

Error AudioDriverSwitch::init_device() {
	int latency = GLOBAL_GET("audio/output_latency");
	mix_rate = GLOBAL_GET("audio/mix_rate");
	channels = 2;
	speaker_mode = SPEAKER_MODE_STEREO;
	buffer_size = closest_power_of_2(latency * mix_rate / 1000);
	samples_in.resize(buffer_size * channels);
	samples_out.resize(buffer_size * channels);

	Result res = audrenInitialize(&arConfig);
	printf("audrenInitialize: %x\n", res);
	// === 补丁 ===
	// 原来这里不检查返回值。蓝牙作为音频输出时 audrenInitialize 会失败，
	// 而失败之后下面所有调用都会失败，最后表现成「音频线程死循环 → App 卡死」。
	// 这里直接报错退出，让引擎回退到 Dummy 驱动（App 能跑，只是没声音）。
	if (R_FAILED(res)) {
		audrenExit();
		return ERR_CANT_OPEN;
	}

	res = audrvCreate(&audren_driver, &arConfig, 2);
	printf("audrvCreate: %x\n", res);
	// === 补丁 ===
	if (R_FAILED(res)) {
		audrenExit();
		return ERR_CANT_OPEN;
	}

	audren_buffer_size = (sizeof(int16_t) * buffer_size * channels);
	audren_pool_size = ((audren_buffer_size * 2) + 0xFFF) & ~0xFFF;
	audren_pool_ptr = memalign(0x1000, audren_pool_size);

	for (int i = 0; i < 2; i++) {
		audren_buffers[i] = { 0 };
		audren_buffers[i].data_raw = audren_pool_ptr;
		audren_buffers[i].size = audren_buffer_size * 2;
		audren_buffers[i].start_sample_offset = i * buffer_size;
		audren_buffers[i].end_sample_offset = audren_buffers[i].start_sample_offset + buffer_size;
	}

	int mpid = audrvMemPoolAdd(&audren_driver, audren_pool_ptr, audren_pool_size);
	audrvMemPoolAttach(&audren_driver, mpid);

	static const u8 sink_channels[] = { 0, 1 };
	audrvDeviceSinkAdd(&audren_driver, AUDREN_DEFAULT_DEVICE_NAME, 2, sink_channels);

	res = audrvUpdate(&audren_driver);
	printf("audrvUpdate: %x\n", res);
	// === 补丁 ===
	if (R_FAILED(res)) {
		audrvClose(&audren_driver);
		audrenExit();
		return ERR_CANT_OPEN;
	}

	res = audrenStartAudioRenderer();
	printf("audrenStartAudioRenderer: %x\n", res);
	// === 补丁 ===
	if (R_FAILED(res)) {
		audrvClose(&audren_driver);
		audrenExit();
		return ERR_CANT_OPEN;
	}

	audrvVoiceInit(&audren_driver, 0, channels, PcmFormat_Int16, mix_rate);
	audrvVoiceSetDestinationMix(&audren_driver, 0, AUDREN_FINAL_MIX_ID);
	if (channels == 1) {
		audrvVoiceSetMixFactor(&audren_driver, 0, 1.0f, 0, 0);
		audrvVoiceSetMixFactor(&audren_driver, 0, 1.0f, 0, 1);
	} else {
		audrvVoiceSetMixFactor(&audren_driver, 0, 1.0f, 0, 0);
		audrvVoiceSetMixFactor(&audren_driver, 0, 0.0f, 0, 1);
		audrvVoiceSetMixFactor(&audren_driver, 0, 0.0f, 1, 0);
		audrvVoiceSetMixFactor(&audren_driver, 0, 1.0f, 1, 1);
	}

	return OK;
}

Error AudioDriverSwitch::init() {
	active = false;
	thread_exited = false;
	exit_thread = false;

	Error err = init_device();
	if (err == OK) {
		thread.start(AudioDriverSwitch::thread_func, this);
	}

	return err;
}

void AudioDriverSwitch::thread_func(void *p_udata) {
	AudioDriverSwitch *ad = (AudioDriverSwitch *)p_udata;

	svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2B);

	while (!ad->exit_thread) {
		ad->lock();
		ad->start_counting_ticks();

		if (!ad->active) {
			for (unsigned int i = 0; i < ad->buffer_size * ad->channels; i++) {
				ad->samples_out.write[i] = 0;
			}
		} else {
			ad->audio_server_process(ad->buffer_size, ad->samples_in.ptrw());
			for (unsigned int i = 0; i < ad->buffer_size * ad->channels; i++) {
				ad->samples_out.write[i] = ad->samples_in[i] >> 16;
			}
		}

		int free_buffer = -1;
		for (int i = 0; i < 2; i++) {
			if (ad->audren_buffers[i].state == AudioDriverWaveBufState_Free || ad->audren_buffers[i].state == AudioDriverWaveBufState_Done) {
				free_buffer = i;
				break;
			}
		}

		if (free_buffer >= 0) {
			uint8_t *ptr = (uint8_t *)ad->audren_pool_ptr + (free_buffer * ad->audren_buffer_size);
			memcpy(ptr, ad->samples_out.ptr(), ad->audren_buffer_size);
			armDCacheFlush(ptr, ad->audren_buffer_size);
			audrvVoiceAddWaveBuf(&ad->audren_driver, 0, &ad->audren_buffers[free_buffer]);
			if (!audrvVoiceIsPlaying(&ad->audren_driver, 0)) {
				audrvVoiceStart(&ad->audren_driver, 0);
			}
			audrvUpdate(&ad->audren_driver);
			audrenWaitFrame();
			// === 补丁 ===
			// 原来这里是：
			//     while (ad->audren_buffers[free_buffer].state != AudioDriverWaveBufState_Playing) {
			//         audrvUpdate(&ad->audren_driver);
			//     }
			// ——没有任何退出条件。蓝牙音频设备变动后缓冲区永远进不了 Playing，
			// 于是这里无限循环；而这个循环持着 ad->lock()，整个 App 就冻住了
			// （真机表现：画面停在最后一帧 / 启动时纯黑屏）。
			//
			// 现在给它一个上限：等不到就放弃这一块，继续往下走。
			// 下一轮会因为没有空闲缓冲区而走原有的「unlock + 歇 1ms + lock」分支，
			// 所以不会空转烧 CPU；设备恢复之后音频有机会自己接着播。
			int wait_tries = 0;
			while (ad->audren_buffers[free_buffer].state != AudioDriverWaveBufState_Playing) {
				audrvUpdate(&ad->audren_driver);
				if (++wait_tries >= SWITCH_AUDIO_WAIT_TRIES) {
					break;
				}
			}
		} else {
			//printf("aud: no free buffer\n");
			ad->stop_counting_ticks();
			ad->unlock();
			OS::get_singleton()->delay_usec(1000);
			ad->lock();
			ad->start_counting_ticks();
		}

		ad->stop_counting_ticks();
		ad->unlock();
	}

	ad->thread_exited = true;
}

void AudioDriverSwitch::start() {
	active = true;
}

int AudioDriverSwitch::get_mix_rate() const {
	return mix_rate;
}

AudioDriver::SpeakerMode AudioDriverSwitch::get_speaker_mode() const {
	return speaker_mode;
}

Array AudioDriverSwitch::get_device_list() {
	Array list;
	list.push_back("Default");
	return list;
}

String AudioDriverSwitch::get_device() {
	return device_name;
}

void AudioDriverSwitch::set_device(String device) {
	lock();
	new_device = device;
	unlock();
}

void AudioDriverSwitch::lock() {
	mutex.lock();
}

void AudioDriverSwitch::unlock() {
	mutex.unlock();
}

void AudioDriverSwitch::finish() {
	exit_thread = true;
	thread.wait_to_finish();

	audrvClose(&audren_driver);
	audrenExit();
}

AudioDriverSwitch::AudioDriverSwitch() :
		device_name("Default"),
		new_device("Default") {
}

AudioDriverSwitch::~AudioDriverSwitch() {
}
