/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  Copyright (C) 2002-2020  The DOSBox Team
 *  Copyright (C) 2020-2020  The dosbox-staging team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "dosbox.h"

#include <array>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <unistd.h>

#include "control.h"
#include "dma.h"
#include "hardware.h"
#include "mixer.h"
#include "pic.h"
#include "setup.h"
#include "shell.h"

#define LOG_GUS 0 // set to 1 for detailed logging

// Global Constants
// ----------------

// AdLib emulation state constant
constexpr uint8_t ADLIB_CMD_DEFAULT = 85u;

// Amplitude level constants
constexpr float ONE_AMP = 1.0f; // first amplitude value
constexpr float AUDIO_SAMPLE_MAX = static_cast<float>(MAX_AUDIO);
constexpr float AUDIO_SAMPLE_MIN = static_cast<float>(MIN_AUDIO);

// Buffer and memory constants
constexpr int BUFFER_FRAMES = 48;
constexpr int BUFFER_SAMPLES = BUFFER_FRAMES * 2; // 2 samples/frame (left & right)
constexpr uint32_t RAM_SIZE = 1048576u;           // 1 MB

// DMA transfer size and rate constants
constexpr uint32_t BYTES_PER_DMA_XFER = 8 * 1024;         // 8 KB per transfer
constexpr uint32_t ISA_BUS_THROUGHPUT = 32 * 1024 * 1024; // 32 MB/s
constexpr uint16_t DMA_TRANSFERS_PER_S = ISA_BUS_THROUGHPUT / BYTES_PER_DMA_XFER;
constexpr float MS_PER_DMA_XFER = 1000.0f / DMA_TRANSFERS_PER_S;

// Voice-channel and state related constants
constexpr uint8_t MAX_VOICES = 32u;
constexpr uint8_t MIN_VOICES = 14u;
constexpr uint8_t VOICE_DEFAULT_STATE = 3u;

// DMA and IRQ extents and quantity constants
constexpr uint8_t MIN_DMA_ADDRESS = 0u;
constexpr uint8_t MAX_DMA_ADDRESS = 7u;
constexpr uint8_t MIN_IRQ_ADDRESS = 0u;
constexpr uint8_t MAX_IRQ_ADDRESS = 15u;
constexpr uint8_t DMA_IRQ_ADDRESSES = 8u; // number of IRQ and DMA channels

// Pan position constants
constexpr uint8_t PAN_DEFAULT_POSITION = 7u;
constexpr uint8_t PAN_POSITIONS = 16u;  // 0: -45-deg, 7: centre, 15: +45-deg

// Timer delay constants
constexpr float TIMER_1_DEFAULT_DELAY = 0.080f;
constexpr float TIMER_2_DEFAULT_DELAY = 0.320f;

// Volume scaling and dampening constants
constexpr auto DELTA_DB = 0.002709201;     // 0.0235 dB increments
constexpr int16_t VOLUME_INC_SCALAR = 512; // Volume index increment scalar
constexpr auto VOLUME_LEVEL_DIVISOR = 1.0 + DELTA_DB;
constexpr uint16_t VOLUME_LEVELS = 4096u;
constexpr float SOFT_LIMIT_RELEASE_INC = AUDIO_SAMPLE_MAX *
                                         static_cast<float>(DELTA_DB);

// Interwave addressing constants
constexpr int16_t WAVE_WIDTH = 1 << 9; // Wave interpolation width (9 bits)
constexpr float WAVE_WIDTH_INV = 1.0f / WAVE_WIDTH;

// IO address quantities
constexpr uint8_t READ_HANDLERS = 8u;
constexpr uint8_t WRITE_HANDLERS = 9u;

// A simple stereo audio frame that's used by the Gus and Voice classes.
struct AudioFrame {
	float left = 0.0f;
	float right = 0.0f;
};

// A group of parameters defining the Gus's voice IRQ control that's also shared
// (as a reference) into each instantiated voice.
struct VoiceIrq {
	uint32_t vol_state = 0u;
	uint32_t wave_state = 0u;
	uint8_t status = 0u;
};

// A group of parameters used in the Voice class to track the Wave and Volume
// controls.
struct VoiceCtrl {
	uint32_t &irq_state;
	int32_t start = 0;
	int32_t end = 0;
	int32_t pos = 0;
	int32_t inc = 0;
	uint16_t rate = 0;
	uint8_t state = VOICE_DEFAULT_STATE;
};

// Collection types involving constant quantities
using address_array_t = std::array<uint8_t, DMA_IRQ_ADDRESSES>;
using autoexec_array_t = std::array<AutoexecObject, 2>;
using read_io_array_t = std::array<IO_ReadHandleObject, READ_HANDLERS>;
using write_io_array_t = std::array<IO_WriteHandleObject, WRITE_HANDLERS>;

// A Voice is used by the Gus class and instantiates 32 of these.
// Each voice represents a single "mono" stream of audio having its own
// characteristics defined by the running program, such as:
//   - being 8bit or 16bit
//   - having a "position" along a left-right axis (panned)
//   - having its volume reduced by some amount (native-level down to 0)
//   - having start, stop, loop, and loop-backward controls
//   - informing the GUS DSP as to when an IRQ is needed to keep it playing
//
class Voice {
public:
	Voice(uint8_t num, VoiceIrq &irq);
	void GenerateSamples(float *stream,
	                     const uint8_t *ram,
	                     const float *vol_scalars,
	                     const AudioFrame *pan_scalars,
	                     const int requested_frames);

	uint8_t ReadVolState() const;
	uint8_t ReadWaveState() const;
	void ResetCtrls();
	void WritePanPot(uint8_t pos);
	void WriteVolRate(uint16_t rate);
	void WriteWaveRate(uint16_t rate);
	bool UpdateVolState(uint8_t state);
	bool UpdateWaveState(uint8_t state);

	VoiceCtrl vol_ctrl;
	VoiceCtrl wave_ctrl;

	uint32_t generated_8bit_ms = 0u;
	uint32_t generated_16bit_ms = 0u;

private:
	Voice() = delete;
	Voice(const Voice &) = delete;            // prevent copying
	Voice &operator=(const Voice &) = delete; // prevent assignment
	bool CheckWaveRolloverCondition();
	bool Is8Bit() const;
	float GetVolScalar(const float *vol_scalars);
	float GetSample(const uint8_t *ram);
	float GetVolumeScalar(const float *vol_scalars) const;
	int32_t PopWavePos();
	int32_t PopVolPos();
	float Read8BitSample(const uint8_t *ram, const int32_t addr) const;
	float Read16BitSample(const uint8_t *ram, const int32_t addr) const;
	uint8_t ReadCtrlState(const VoiceCtrl &ctrl) const;
	void IncrementCtrlPos(VoiceCtrl &ctrl, bool skip_loop);
	bool UpdateCtrlState(VoiceCtrl &ctrl, uint8_t state);

	// Control states
	enum CTRL : uint8_t {
		RESET = 0x01,
		STOPPED = 0x02,
		DISABLED = RESET | STOPPED,
		BIT16 = 0x04,
		LOOP = 0x08,
		BIDIRECTIONAL = 0x10,
		RAISEIRQ = 0x20,
		DECREASING = 0x40,
	};

	uint32_t irq_mask = 0u;
	uint8_t &shared_irq_status;
	uint8_t pan_position = PAN_DEFAULT_POSITION;
};

static void GUS_TimerEvent(Bitu t);
static void GUS_DMA_Event(Bitu val);

using voice_array_t = std::array<std::unique_ptr<Voice>, MAX_VOICES>;

// The Gravis UltraSound GF1 DSP (classic)
// This class:
//   - Registers, receives, and responds to port address inputs, which are used
//     by the emulated software to configure and control the GUS card.
//   - Reads or provides audio samples via direct memory access (DMA)
//   - Provides shared resources to all of the Voices, such as the volume
//     reducing table, constant-power panning table, and IRQ states.
//   - Integrates the audio from each active voice into a 16-bit stereo output
//     stream without resampling.
//   - Populates an autoexec line (ULTRASND=...) with its port, irq, and dma
//     addresses.
//
class Gus {
public:
	Gus(uint16_t port, uint8_t dma, uint8_t irq, const std::string &dir);
	bool CheckTimer(size_t t);
	void PrintStats();

	struct Timer {
		float delay = 0.0f;
		uint8_t value = 0xff;
		bool has_expired = true;
		bool is_counting_down = false;
		bool is_masked = false;
		bool should_raise_irq = false;
	};
	Timer timers[2] = {{TIMER_1_DEFAULT_DELAY}, {TIMER_2_DEFAULT_DELAY}};
	bool PerformDmaTransfer();

private:
	Gus() = delete;
	Gus(const Gus &) = delete;            // prevent copying
	Gus &operator=(const Gus &) = delete; // prevent assignment

	void ActivateVoices(uint8_t requested_voices);
	void AudioCallback(uint16_t requested_frames);
	void BeginPlayback();
	void CheckIrq();
	void CheckVoiceIrq();
	uint32_t Dma8Addr();
	uint32_t Dma16Addr();
	void DmaCallback(DmaChannel *chan, DMAEvent event);
	void StartDmaTransfers();
	bool IsDmaPcm16Bit();
	bool IsDmaXfer16Bit();
	uint16_t ReadFromRegister();
	void PopulateAutoExec(uint16_t port, const std::string &dir);
	void PopulatePanScalars();
	void PopulateVolScalars();
	void PrepareForPlayback();
	size_t ReadFromPort(const size_t port, const size_t iolen);
	void RegisterIoHandlers();
	void Reset(uint8_t state);
	void SoftLimit(const float *in, int16_t *out);
	void StopPlayback();
	void UpdateDmaAddress(uint8_t new_address);
	void UpdateWaveMsw(int32_t &addr) const;
	void UpdateWaveLsw(int32_t &addr) const;
	void UpdatePeakAmplitudes(const float *stream);
	void WriteToPort(size_t port, size_t val, size_t iolen);
	void WriteToRegister();

	// Collections
	float vol_scalars[VOLUME_LEVELS] = {};
	float accumulator[BUFFER_SAMPLES] = {0};
	int16_t scaled[BUFFER_SAMPLES] = {};
	AudioFrame pan_scalars[PAN_POSITIONS] = {};
	uint8_t ram[RAM_SIZE] = {0u};
	read_io_array_t read_handlers = {};   // std::functions
	write_io_array_t write_handlers = {}; // std::functions
	const address_array_t dma_addresses = {
	        {MIN_DMA_ADDRESS, 1, 3, 5, 6, MAX_IRQ_ADDRESS, 0, 0}};
	const address_array_t irq_addresses = {
	        {MIN_IRQ_ADDRESS, 2, 5, 3, 7, 11, 12, MAX_IRQ_ADDRESS}};
	voice_array_t voices = {{nullptr}};
	autoexec_array_t autoexec_lines = {};

	// Struct and pointer members
	VoiceIrq voice_irq = {};
	MixerObject mixer_channel = {};
	AudioFrame peak = {ONE_AMP, ONE_AMP};
	Voice *voice = nullptr;
	DmaChannel *dma_channel = nullptr;
	MixerChannel *audio_channel = nullptr;
	uint8_t &adlib_command_reg = adlib_commandreg;

	// Port address
	size_t port_base = 0u;

	// Voice states
	uint32_t active_voice_mask = 0u;
	uint16_t voice_index = 0u;
	uint8_t active_voices = 0u;
	uint8_t prev_logged_voices = 0u;

	// Register and playback rate
	uint32_t dram_addr = 0u;
	uint32_t playback_rate = 0u;
	uint16_t register_data = 0u;
	uint8_t selected_register = 0u;

	// Control states
	uint8_t mix_ctrl = 0x0b; // latches enabled, LINEs disabled
	uint8_t sample_ctrl = 0u;
	uint8_t timer_ctrl = 0u;

	// DMA states
	uint16_t dma_addr = 0u;
	uint8_t dma_ctrl = 0u;
	uint8_t dma1 = 0u; // playback DMA
	uint8_t dma2 = 0u; // recording DMA

	// IRQ states
	uint8_t irq1 = 0u; // playback IRQ
	uint8_t irq2 = 0u; // MIDI IRQ
	uint8_t irq_status = 0u;
	bool irq_enabled = false;
	bool should_change_irq_dma = false;
};

using namespace std::placeholders;

// External Tie-in for OPL FM-audio
uint8_t adlib_commandreg = ADLIB_CMD_DEFAULT;

static std::unique_ptr<Gus> gus = nullptr;

Voice::Voice(uint8_t num, VoiceIrq &irq)
        : vol_ctrl{irq.vol_state},
          wave_ctrl{irq.wave_state},
          irq_mask(1 << num),
          shared_irq_status(irq.status)

{}

/*
Gravis SDK, Section 3.11. Rollover feature:
	Each voice has a 'rollover' feature that allows an application to be notified
	when a voice's playback position passes over a particular place in DRAM.  This
	is very useful for getting seamless digital audio playback.  Basically, the GF1
	will generate an IRQ when a voice's current position is  equal to the end
	position.  However, instead of stopping or looping back to the start position,
	the voice will continue playing in the same direction.  This means that there
	will be no pause (or gap) in the playback.

	Note that this feature is enabled/disabled through the voice's VOLUME control
	register (since there are no more bits available in the voice control
	registers).   A voice's loop enable bit takes precedence over the rollover. This
	means that if a voice's loop enable is on, it will loop when it hits the end
	position, regardless of the state of the rollover enable.
---
Joh Campbell, maintainer of DOSox-X:
	Despite the confusing description above, that means that looping takes
	precedence over rollover. If not looping, then rollover means to fire the IRQ
	but keep moving. If looping, then fire IRQ and carry out loop behavior. Gravis
	Ultrasound Windows 3.1 drivers expect this behavior, else Windows WAVE output
	will not work correctly.
*/
bool Voice::CheckWaveRolloverCondition()
{
	return (vol_ctrl.state & CTRL::BIT16) && !(wave_ctrl.state & CTRL::LOOP);
}

void Voice::IncrementCtrlPos(VoiceCtrl &ctrl, bool dont_loop_or_restart)
{
	if (ctrl.state & CTRL::DISABLED)
		return;
	int32_t remaining;
	if (ctrl.state & CTRL::DECREASING) {
		ctrl.pos -= ctrl.inc;
		remaining = ctrl.start - ctrl.pos;
	} else {
		ctrl.pos += ctrl.inc;
		remaining = ctrl.pos - ctrl.end;
	}
	// Not yet reaching a boundary
	if (remaining < 0)
		return;

	// Generate an IRQ if requested
	if (ctrl.state & CTRL::RAISEIRQ) {
		ctrl.irq_state |= irq_mask;
	}

	// Allow the current position to move beyond its limit
	if (dont_loop_or_restart)
		return;

	// Should we loop?
	if (ctrl.state & CTRL::LOOP) {
		/* Bi-directional looping */
		if (ctrl.state & CTRL::BIDIRECTIONAL)
			ctrl.state ^= CTRL::DECREASING;
		ctrl.pos = (ctrl.state & CTRL::DECREASING)
		                   ? ctrl.end - remaining
		                   : ctrl.start + remaining;
	}
	// Otherwise, restart the position back to its start or end
	else {
		ctrl.state |= 1; // Stop the voice
		ctrl.pos = (ctrl.state & CTRL::DECREASING) ? ctrl.start : ctrl.end;
	}
	return;
}

bool Voice::Is8Bit() const
{
	return !(wave_ctrl.state & CTRL::BIT16);
}

float Voice::GetSample(const uint8_t *ram)
{
	const int32_t pos = PopWavePos();
	const auto addr = pos / WAVE_WIDTH;
	const auto fraction = pos & (WAVE_WIDTH - 1);
	const bool should_interpolate = wave_ctrl.inc < WAVE_WIDTH && fraction;
	float sample = Is8Bit() ? Read8BitSample(ram, addr)
	                        : Read16BitSample(ram, addr);
	if (should_interpolate) {
		const auto next_addr = addr + 1;
		const float next_sample = Is8Bit() ? Read8BitSample(ram, next_addr)
		                                   : Read16BitSample(ram, next_addr);
		sample += (next_sample - sample) *
		          static_cast<float>(fraction) * WAVE_WIDTH_INV;
	}
	assert(sample >= AUDIO_SAMPLE_MIN && sample <= AUDIO_SAMPLE_MAX);
	return sample;
}

float Voice::GetVolScalar(const float *vol_scalars)
{
	// Unscale the volume index and check its bounds
	const auto i = static_cast<size_t>(
	        ceil_sdivide(PopVolPos(), VOLUME_INC_SCALAR));
	assert(i < VOLUME_LEVELS);
	const float scalar = vol_scalars[i];
	return scalar;
}

void Voice::GenerateSamples(float *stream,
                            const uint8_t *ram,
                            const float *vol_scalars,
                            const AudioFrame *pan_scalars,
                            const int requested_frames)
{
	if (vol_ctrl.state & wave_ctrl.state & CTRL::DISABLED)
		return;

	// Add the samples to the stream, angled in L-R space
	const int sample_end = requested_frames * 2 - 1;
	for (int i = 0; i < sample_end; i += 2) {
		float sample = GetSample(ram);
		sample *= GetVolScalar(vol_scalars);
		stream[i] += sample * pan_scalars[pan_position].left;
		stream[i + 1] += sample * pan_scalars[pan_position].right;
	}
	// Keep track of how many ms this voice has generated
	Is8Bit() ? generated_8bit_ms++ : generated_16bit_ms++;
}

int32_t Voice::PopWavePos()
{
	const int32_t pos = wave_ctrl.pos;
	IncrementCtrlPos(wave_ctrl, CheckWaveRolloverCondition());
	return pos;
}

int32_t Voice::PopVolPos()
{
	const int32_t pos = vol_ctrl.pos;
	IncrementCtrlPos(vol_ctrl, false); // don't check wave rollover
	return pos;
}

// Read an 8-bit sample scaled into the 16-bit range, returned as a float
float Voice::Read8BitSample(const uint8_t *ram, const int32_t addr) const
{
	constexpr float to_16bit_range = 1u
	                                 << (std::numeric_limits<int16_t>::digits -
	                                     std::numeric_limits<int8_t>::digits);
	const size_t i = static_cast<uint32_t>(addr) & 0xFFFFFu;
	assert(i < RAM_SIZE);
	return static_cast<int8_t>(ram[i]) * to_16bit_range;
}

// Read a 16-bit sample returned as a float
float Voice::Read16BitSample(const uint8_t *ram, const int32_t addr) const
{
	// Calculate offset of the 16-bit sample
	const auto lower = static_cast<unsigned>(addr) & 0xC0000u;
	const auto upper = static_cast<unsigned>(addr) & 0x1FFFFu;
	const size_t i = lower | (upper << 1);
	assert(i < RAM_SIZE);
	return static_cast<int16_t>(host_readw(ram + i));
}

uint8_t Voice::ReadCtrlState(const VoiceCtrl &ctrl) const
{
	uint8_t state = ctrl.state;
	if (ctrl.irq_state & irq_mask)
		state |= 0x80;
	return state;
}

uint8_t Voice::ReadVolState() const
{
	return ReadCtrlState(vol_ctrl);
}

uint8_t Voice::ReadWaveState() const
{
	return ReadCtrlState(wave_ctrl);
}

void Voice::ResetCtrls()
{
	vol_ctrl.pos = 0u;
	UpdateVolState(0x1);
	UpdateWaveState(0x1);
	WritePanPot(PAN_DEFAULT_POSITION);
}

bool Voice::UpdateCtrlState(VoiceCtrl &ctrl, uint8_t state)
{
	const uint32_t orig_irq_state = ctrl.irq_state;
	ctrl.state = state & 0x7f;
	// Manually set the irq
	if ((state & 0xa0) == 0xa0)
		ctrl.irq_state |= irq_mask;
	else
		ctrl.irq_state &= ~irq_mask;

	// Indicate if the IRQ state changed
	return orig_irq_state != ctrl.irq_state;
}

bool Voice::UpdateVolState(uint8_t state)
{
	return UpdateCtrlState(vol_ctrl, state);
}

bool Voice::UpdateWaveState(uint8_t state)
{
	return UpdateCtrlState(wave_ctrl, state);
}

void Voice::WritePanPot(uint8_t pos)
{
	constexpr uint8_t max_pos = PAN_POSITIONS - 1;
	pan_position = std::min(pos, max_pos);
}

// Four volume-index-rate "banks" are available that define the number of
// volume indexes that will be incremented (or decremented, depending on the
// volume_ctrl value) each step, for a given voice.  The banks are:
//
// - 0 to 63, which defines single index increments,
// - 64 to 127 defines fractional index increments by 1/8th,
// - 128 to 191 defines fractional index increments by 1/64ths, and
// - 192 to 255 defines fractional index increments by 1/512ths.
//
// To ensure the smallest increment (1/512) effects an index change, we
// normalize all the volume index variables (including this) by multiplying by
// VOLUME_INC_SCALAR (or 512). Note that "index" qualifies all these variables
// because they are merely indexes into the vol_scalars[] array. The actual
// volume scalar value (a floating point fraction between 0.0 and 1.0) is never
// actually operated on, and is simply looked up from the final index position
// at the time of sample population.
void Voice::WriteVolRate(uint16_t val)
{
	vol_ctrl.rate = val;
	constexpr uint8_t bank_lengths = 63;
	const int pos_in_bank = val & bank_lengths;
	const int decimator = 1 << (3 * (val >> 6));
	vol_ctrl.inc = ceil_sdivide(pos_in_bank * VOLUME_INC_SCALAR, decimator);

	// Sanity check the bounds of the incrementer
	assert(vol_ctrl.inc >= 0 && vol_ctrl.inc <= bank_lengths * VOLUME_INC_SCALAR);
}

void Voice::WriteWaveRate(uint16_t val)
{
	wave_ctrl.rate = val;
	wave_ctrl.inc = ceil_udivide(val, 2u);
}

Gus::Gus(uint16_t port, uint8_t dma, uint8_t irq, const std::string &ultradir)
        : port_base(port - 0x200),
          dma2(dma),
          irq1(irq),
          irq2(irq)
{
	// Create the internal voice channels
	for (uint8_t i = 0; i < MAX_VOICES; ++i) {
		voices.at(i) = std::make_unique<Voice>(i, voice_irq);
	}

	RegisterIoHandlers();

	// Register the Audio and DMA callbacks
	audio_channel = mixer_channel.Install(
		std::bind(&Gus::AudioCallback, this, std::placeholders::_1), 1, "GUS");

	UpdateDmaAddress(dma);

	// Populate the volume, pan, and auto-exec arrays
	PopulateVolScalars();
	PopulatePanScalars();
	PopulateAutoExec(port, ultradir);
}

void Gus::ActivateVoices(uint8_t requested_voices)
{
	requested_voices = clamp(requested_voices, MIN_VOICES, MAX_VOICES);
	if (requested_voices != active_voices) {
		active_voices = requested_voices;
		assert(active_voices <= voices.size());
		active_voice_mask = 0xffffffffU >> (MAX_VOICES - active_voices);
		playback_rate = static_cast<uint32_t>(
		        0.5 + 1000000.0 / (1.619695497 * active_voices));
		audio_channel->SetFreq(playback_rate);
	}
}

void Gus::AudioCallback(const uint16_t requested_frames)
{
	assert(requested_frames <= BUFFER_FRAMES);

	// Zero the accumulator array
	for (int i = 0; i < BUFFER_SAMPLES; ++i)
		accumulator[i] = 0;

	for (uint8_t i = 0; i < active_voices; ++i)
		voices[i]->GenerateSamples(accumulator, ram, vol_scalars,
		                           pan_scalars, requested_frames);

	SoftLimit(accumulator, scaled);
	audio_channel->AddSamples_s16(requested_frames, scaled);
	CheckVoiceIrq();
}

void Gus::BeginPlayback()
{
	audio_channel->Enable(true);
	if (prev_logged_voices != active_voices) {
		LOG_MSG("GUS: Activated %u voices at %u Hz", active_voices,
		        playback_rate);
		prev_logged_voices = active_voices;
	}
}

void Gus::CheckIrq()
{
	if (irq_status && (mix_ctrl & 0x08))
		PIC_ActivateIRQ(irq1);
}

bool Gus::CheckTimer(const size_t t)
{
	if (!timers[t].is_masked)
		timers[t].has_expired = true;
	if (timers[t].should_raise_irq) {
		irq_status |= 0x4 << t;
		CheckIrq();
	}
	return timers[t].is_counting_down;
}

void Gus::CheckVoiceIrq()
{
	irq_status &= 0x9f;
	const Bitu totalmask = (voice_irq.vol_state | voice_irq.wave_state) &
	                       active_voice_mask;
	if (!totalmask)
		return;
	if (voice_irq.vol_state)
		irq_status |= 0x40;
	if (voice_irq.wave_state)
		irq_status |= 0x20;
	CheckIrq();
	while (!(totalmask & 1ULL << voice_irq.status)) {
		voice_irq.status++;
		if (voice_irq.status >= active_voices)
			voice_irq.status = 0;
	}
}

uint32_t Gus::Dma8Addr()
{
	return static_cast<uint32_t>(dma_addr << 4);
}

uint32_t Gus::Dma16Addr()
{
	const auto lower = dma_addr & 0x1fff;
	const auto upper = dma_addr & 0xc000;
	const auto combined = (lower << 1) | upper;
	return static_cast<uint32_t>(combined << 4);
}

bool Gus::PerformDmaTransfer()
{
	if (dma_channel->masked || !(dma_ctrl & 0x01))
		return false;

#if LOG_GUS
	LOG_MSG("GUS DMA event: max %u bytes. DMA: tc=%u mask=0 cnt=%u",
	        BYTES_PER_DMA_XFER, dma_channel->tcount ? 1 : 0,
	        dma_channel->currcnt + 1);
#endif

	const auto addr = IsDmaXfer16Bit() ? Dma16Addr() : Dma8Addr();
	const uint16_t desired = dma_channel->currcnt + 1;

	if ((dma_ctrl & 0x2)) // Copy samples via DMA from GUS memory
		dma_channel->Write(desired, ram + addr);

	else if (!(dma_ctrl & 0x80)) // Skip DMA content
		dma_channel->Read(desired, ram + addr);

	else { // Copy samples via DMA into GUS memory
		const auto samples = dma_channel->Read(desired, ram + addr);
		const auto start = addr + (IsDmaPcm16Bit() ? 1u : 0u);
		const auto skip = IsDmaPcm16Bit() ? 2u : 1u;
		const auto end = addr + samples * (dma_channel->DMA16 + 1u);
		for (size_t i = start; i < end; i += skip)
			ram[i] ^= 0x80;
	}
	// Raise the TC irq if needed
	if ((dma_ctrl & 0x20) != 0) {
		irq_status |= 0x80;
		CheckIrq();
		return false;
	}
	return true;
}

bool Gus::IsDmaPcm16Bit()
{
	return dma_ctrl & 0x40;
}

bool Gus::IsDmaXfer16Bit()
{
	// What bit-size should DMA memory be transferred as?
	// Mode PCM/DMA  Address Use-16  Note
	// 0x00   8/ 8   Any     No      Most DOS programs
	// 0x04   8/16   >= 4    Yes     16-bit if using High DMA
	// 0x04   8/16   < 4     No      8-bit if using Low DMA
	// 0x40  16/ 8   Any     No      Windows 3.1, Quake
	// 0x44  16/16   >= 4    Yes     Windows 3.1, Quake
	return (dma_ctrl & 0x4) && (dma1 >= 4);
}

static void GUS_DMA_Event(Bitu)
{
	if (gus->PerformDmaTransfer())
		PIC_AddEvent(GUS_DMA_Event, MS_PER_DMA_XFER);
}

void Gus::StartDmaTransfers()
{
	PIC_AddEvent(GUS_DMA_Event, MS_PER_DMA_XFER);
}

void Gus::DmaCallback(DmaChannel *, DMAEvent event)
{
	if (event == DMA_UNMASKED)
		StartDmaTransfers();
}

void Gus::PopulateAutoExec(uint16_t port, const std::string &ultradir)
{
	// ULTRASND=Port,(rec)DMA1,(pcm)DMA2,(play)IRQ1,(midi)IRQ2
	std::ostringstream sndline;
	sndline << "SET ULTRASND=" << std::hex << std::setw(3) << port << ","
	        << std::dec << static_cast<int>(dma1) << ","
	        << static_cast<int>(dma2) << "," << static_cast<int>(irq1)
	        << "," << static_cast<int>(irq2) << std::ends;
	LOG_MSG("GUS: %s", sndline.str().c_str());
	autoexec_lines.at(0).Install(sndline.str());

	// ULTRADIR=full path to directory containing "midi"
	std::string dirline = "SET ULTRADIR=" + ultradir;
	autoexec_lines.at(1).Install(dirline);
}

// Generate logarithmic to linear volume conversion tables
void Gus::PopulateVolScalars()
{
	double out = 1.0;
	for (uint16_t i = VOLUME_LEVELS - 1; i > 0; --i) {
		vol_scalars[i] = static_cast<float>(out);
		out /= VOLUME_LEVEL_DIVISOR;
	}
	vol_scalars[0] = 0.0f;
}

/*
Constant-Power Panning
-------------------------
The GUS SDK describes having 16 panning positions (0 through 15)
with 0 representing the full-left rotation, 7 being the mid-point,
and 15 being the full-right rotation.  The SDK also describes
that output power is held constant through this range.

	#!/usr/bin/env python3
	import math
	print(f'Left-scalar  Pot Norm.   Right-scalar | Power')
	print(f'-----------  --- -----   ------------ | -----')
	for pot in range(16):
		norm = (pot - 7.) / (7.0 if pot < 7 else 8.0)
		direction = math.pi * (norm + 1.0 ) / 4.0
		lscale = math.cos(direction)
		rscale = math.sin(direction)
		power = lscale * lscale + rscale * rscale
		print(f'{lscale:.5f} <~~~ {pot:2} ({norm:6.3f})'\
		      f' ~~~> {rscale:.5f} | {power:.3f}')

	Left-scalar  Pot Norm.   Right-scalar | Power
	-----------  --- -----   ------------ | -----
	1.00000 <~~~  0 (-1.000) ~~~> 0.00000 | 1.000
	0.99371 <~~~  1 (-0.857) ~~~> 0.11196 | 1.000
	0.97493 <~~~  2 (-0.714) ~~~> 0.22252 | 1.000
	0.94388 <~~~  3 (-0.571) ~~~> 0.33028 | 1.000
	0.90097 <~~~  4 (-0.429) ~~~> 0.43388 | 1.000
	0.84672 <~~~  5 (-0.286) ~~~> 0.53203 | 1.000
	0.78183 <~~~  6 (-0.143) ~~~> 0.62349 | 1.000
	0.70711 <~~~  7 ( 0.000) ~~~> 0.70711 | 1.000
	0.63439 <~~~  8 ( 0.125) ~~~> 0.77301 | 1.000
	0.55557 <~~~  9 ( 0.250) ~~~> 0.83147 | 1.000
	0.47140 <~~~ 10 ( 0.375) ~~~> 0.88192 | 1.000
	0.38268 <~~~ 11 ( 0.500) ~~~> 0.92388 | 1.000
	0.29028 <~~~ 12 ( 0.625) ~~~> 0.95694 | 1.000
	0.19509 <~~~ 13 ( 0.750) ~~~> 0.98079 | 1.000
	0.09802 <~~~ 14 ( 0.875) ~~~> 0.99518 | 1.000
	0.00000 <~~~ 15 ( 1.000) ~~~> 1.00000 | 1.000
*/
void Gus::PopulatePanScalars()
{
	for (int i = 0; i < PAN_POSITIONS; ++i) { // Vectorized
		// Normalize absolute range [0, 15] to [-1.0, 1.0]
		const auto norm = (i - 7.0) / (i < 7 ? 7 : 8);
		// Convert to an angle between 0 and 90-degree, in radians
		const auto angle = (norm + 1) * M_PI / 4;
		pan_scalars[i].left = static_cast<float>(cos(angle));
		pan_scalars[i].right = static_cast<float>(sin(angle));
		// DEBUG_LOG_MSG("GUS: pan_scalar[%u] = %f | %f", i,
		//               pan_scalars.at(i).left,
		//               pan_scalars.at(i).right);
	}
}

void Gus::PrepareForPlayback()
{
	// Initialize the voice states
	for (auto &v : voices)
		v->ResetCtrls();

	// Initialize the OPL emulator state
	adlib_command_reg = ADLIB_CMD_DEFAULT;

	voice_irq = VoiceIrq{};
	timers[0] = Timer{TIMER_1_DEFAULT_DELAY};
	timers[1] = Timer{TIMER_2_DEFAULT_DELAY};
}

void Gus::PrintStats()
{
	// Aggregate stats from all voices
	uint32_t combined_8bit_ms = 0u;
	uint32_t combined_16bit_ms = 0u;
	uint32_t used_8bit_voices = 0u;
	uint32_t used_16bit_voices = 0u;
	for (const auto &v : voices) {
		if (v->generated_8bit_ms) {
			combined_8bit_ms += v->generated_8bit_ms;
			used_8bit_voices++;
		}
		if (v->generated_16bit_ms) {
			combined_16bit_ms += v->generated_16bit_ms;
			used_16bit_voices++;
		}
	}
	const uint32_t combined_ms = combined_8bit_ms + combined_16bit_ms;

	// Is there enough information to be meaningful?
	if (combined_ms < 10000u || (peak.left + peak.right) < 10 ||
	    !(used_8bit_voices + used_16bit_voices))
		return;

	// Print info about the type of audio and voices used
	if (used_16bit_voices == 0u)
		LOG_MSG("GUS: Audio comprised of 8-bit samples from %u voices",
		        used_8bit_voices);
	else if (used_8bit_voices == 0u)
		LOG_MSG("GUS: Audio comprised of 16-bit samples from %u voices",
		        used_16bit_voices);
	else {
		const auto ratio_8bit = ceil_udivide(100u * combined_8bit_ms,
		                                     combined_ms);
		const auto ratio_16bit = ceil_udivide(100u * combined_16bit_ms,
		                                      combined_ms);
		LOG_MSG("GUS: Audio was made up of %u%% 8-bit %u-voice and "
		        "%u%% 16-bit %u-voice samples",
		        ratio_8bit, used_8bit_voices, ratio_16bit,
		        used_16bit_voices);
	}

	// Calculate and print info about the volume
	const auto mixer_scalar = std::max(audio_channel->volmain[0],
	                                   audio_channel->volmain[1]);
	const auto peak_sample = std::max(peak.left, peak.right);
	auto peak_ratio = mixer_scalar * peak_sample / AUDIO_SAMPLE_MAX;

	// It's expected and normal for multi-voice audio to periodically
	// accumulate beyond the max, which is gracefully scaled without
	// distortion, so there is no need to recommend that users scale-down
	// their GUS mixer settings.
	peak_ratio = std::min(peak_ratio, 1.0f);
	LOG_MSG("GUS: Peak amplitude reached %.0f%% of max",
	        static_cast<double>(100 * peak_ratio));

	// Make a suggestion if the peak volume was well below 3 dB
	if (peak_ratio < 0.6f) {
		const auto multiplier = static_cast<uint16_t>(
		        100 * mixer_scalar / peak_ratio);
		LOG_MSG("GUS: If it should be louder, %s %u",
		        static_cast<float>(fabs(mixer_scalar - 1.0f)) > 0.01f
		                ? "adjust mixer gus to"
		                : "use: mixer gus",
		        multiplier);
	}
}

Bitu Gus::ReadFromPort(const Bitu port, const Bitu iolen)
{
	//	LOG_MSG("GUS: Read from port %x", port);
	switch (port - port_base) {
	case 0x206: return irq_status;
	case 0x208:
		uint8_t time;
		time = 0u;
		if (timers[0].has_expired)
			time |= (1 << 6);
		if (timers[1].has_expired)
			time |= (1 << 5);
		if (time & 0x60)
			time |= (1 << 7);
		if (irq_status & 0x04)
			time |= (1 << 2);
		if (irq_status & 0x08)
			time |= (1 << 1);
		return time;
	case 0x20a: return adlib_command_reg;
	case 0x302: return static_cast<uint8_t>(voice_index);
	case 0x303: return selected_register;
	case 0x304:
		if (iolen == 2)
			return ReadFromRegister() & 0xffff;
		else
			return ReadFromRegister() & 0xff;
	case 0x305: return ReadFromRegister() >> 8;
	case 0x307:
		if (dram_addr < RAM_SIZE) {
			return ram[dram_addr];
		} else {
			return 0;
		}
	default:
#if LOG_GUS
		LOG_MSG("GUS Read at port 0x%x", port);
#endif
		break;
	}

	return 0xff;
}

uint16_t Gus::ReadFromRegister()
{
	// LOG_MSG("GUS: Read register %x", selected_register);
	uint8_t reg;

	// Registers that read from the general DSP
	switch (selected_register) {
	case 0x41: // Dma control register - read acknowledges DMA IRQ
		reg = dma_ctrl & 0xbf;
		reg |= (irq_status & 0x80) >> 1;
		irq_status &= 0x7f;
		CheckIrq();
		return static_cast<uint16_t>(reg << 8);
	case 0x42: // Dma address register
		return dma_addr;
	case 0x45: // Timer control register matches Adlib's behavior
		return static_cast<uint16_t>(timer_ctrl << 8);
	case 0x49: // Dma sample register
		reg = dma_ctrl & 0xbf;
		reg |= (irq_status & 0x80) >> 1;
		return static_cast<uint16_t>(reg << 8);
	case 0x8f: // General voice IRQ status register
		reg = voice_irq.status | 0x20;
		uint32_t mask;
		mask = 1 << voice_irq.status;
		if (!(voice_irq.vol_state & mask))
			reg |= 0x40;
		if (!(voice_irq.wave_state & mask))
			reg |= 0x80;
		voice_irq.vol_state &= ~mask;
		voice_irq.wave_state &= ~mask;
		CheckVoiceIrq();
		return static_cast<uint16_t>(reg << 8);
	}

	if (!voice)
		return (selected_register == 0x80 || selected_register == 0x8d)
		               ? 0x0300
		               : 0u;

	// Registers that read from from the current voice
	switch (selected_register) {
	case 0x80: // Voice wave control read register
		return static_cast<uint16_t>(voice->ReadWaveState() << 8);
	case 0x82: // Voice MSB start address register
		return static_cast<uint16_t>(voice->wave_ctrl.start >> 16);
	case 0x83: // Voice LSW start address register
		return static_cast<uint16_t>(voice->wave_ctrl.start);
	case 0x89: // Voice volume register
	{
		const int i = ceil_sdivide(voice->vol_ctrl.pos, VOLUME_INC_SCALAR);
		assert(i < VOLUME_LEVELS);
		return static_cast<uint16_t>(i << 4);
	}
	case 0x8a: // Voice MSB current address register
		return static_cast<uint16_t>(voice->wave_ctrl.pos >> 16);
	case 0x8b: // Voice LSW current address register
		return static_cast<uint16_t>(voice->wave_ctrl.pos);
	case 0x8d: // Voice volume control register
		return static_cast<uint16_t>(voice->ReadVolState() << 8);
	}
#if LOG_GUS
	LOG_MSG("GUS: Unimplemented read Register 0x%x", selected_register);
#endif
	return register_data;
}

void Gus::RegisterIoHandlers()
{
	// Register the IO read addresses
	assert(7 < read_handlers.size());
	const auto read_from = std::bind(&Gus::ReadFromPort, this, _1, _2);
	read_handlers[0].Install(0x302 + port_base, read_from, IO_MB);
	read_handlers[1].Install(0x303 + port_base, read_from, IO_MB);
	read_handlers[2].Install(0x304 + port_base, read_from, IO_MB | IO_MW);
	read_handlers[3].Install(0x305 + port_base, read_from, IO_MB);
	read_handlers[4].Install(0x206 + port_base, read_from, IO_MB);
	read_handlers[5].Install(0x208 + port_base, read_from, IO_MB);
	read_handlers[6].Install(0x307 + port_base, read_from, IO_MB);
	// Board Only
	read_handlers[7].Install(0x20A + port_base, read_from, IO_MB);

	// Register the IO write addresses
	// We'll leave the MIDI interface to the MPU-401
	// Ditto for the Joystick
	// GF1 Synthesizer
	assert(8 < write_handlers.size());
	const auto write_to = std::bind(&Gus::WriteToPort, this, _1, _2, _3);
	write_handlers[0].Install(0x302 + port_base, write_to, IO_MB);
	write_handlers[1].Install(0x303 + port_base, write_to, IO_MB);
	write_handlers[2].Install(0x304 + port_base, write_to, IO_MB | IO_MW);
	write_handlers[3].Install(0x305 + port_base, write_to, IO_MB);
	write_handlers[4].Install(0x208 + port_base, write_to, IO_MB);
	write_handlers[5].Install(0x209 + port_base, write_to, IO_MB);
	write_handlers[6].Install(0x307 + port_base, write_to, IO_MB);
	// Board Only
	write_handlers[7].Install(0x200 + port_base, write_to, IO_MB);
	write_handlers[8].Install(0x20B + port_base, write_to, IO_MB);
}

void Gus::StopPlayback()
{
	// Halt playback before altering the DSP state
	audio_channel->Enable(false);

	irq_enabled = false;
	irq_status = 0;

	dma_ctrl = 0u;
	mix_ctrl = 0xb; // latches enabled, LINEs disabled
	timer_ctrl = 0u;
	sample_ctrl = 0u;

	voice = nullptr;
	voice_index = 0u;
	active_voices = 0u;

	dma_addr = 0u;
	dram_addr = 0u;
	register_data = 0u;
	selected_register = 0u;
	should_change_irq_dma = false;
	PIC_RemoveEvents(GUS_TimerEvent);
}

void Gus::SoftLimit(const float *in, int16_t *out)
{
	UpdatePeakAmplitudes(in);

	// If our peaks are under the max, then there's no need to limit
	if (peak.left < AUDIO_SAMPLE_MAX && peak.right < AUDIO_SAMPLE_MAX) {
		for (int i = 0; i < BUFFER_SAMPLES - 1; i += 2) {
			out[i] = static_cast<int16_t>(in[i]);
			out[i + 1] = static_cast<int16_t>(in[i + 1]);
		}
		return;
	}
	// Calculate the percent we need to scale down the volume index
	// position.  In cases where one side is less than the max, it's ratio
	// is limited to 1.0.
	const float left_scalar = std::min(ONE_AMP, AUDIO_SAMPLE_MAX / peak.left);
	const float right_scalar = std::min(ONE_AMP, AUDIO_SAMPLE_MAX / peak.right);

	for (int i = 0; i < BUFFER_SAMPLES - 1; i += 2) { // Vectorized
		out[i] = static_cast<int16_t>(in[i] * left_scalar);
		out[i + 1] = static_cast<int16_t>(in[i + 1] * right_scalar);
	}
	if (peak.left > AUDIO_SAMPLE_MAX)
		peak.left -= SOFT_LIMIT_RELEASE_INC;
	if (peak.right > AUDIO_SAMPLE_MAX)
		peak.right -= SOFT_LIMIT_RELEASE_INC;
	// LOG_MSG("GUS: releasing peak_amplitude = %.2f | %.2f",
	//         static_cast<double>(peak.left),
	//         static_cast<double>(peak.right));
}

static void GUS_TimerEvent(Bitu t)
{
	if (gus->CheckTimer(t))
		PIC_AddEvent(GUS_TimerEvent, gus->timers[t].delay, t);
}

void Gus::UpdateDmaAddress(const uint8_t new_address)
{
	// Has it changed?
	if (new_address == dma1)
		return;

	// Unregister the current callback
	if (dma_channel)
		dma_channel->Register_Callback(nullptr);

	// Update the address, channel, and callback
	dma1 = new_address;
	dma_channel = GetDMAChannel(dma1);
	assert(dma_channel);
	dma_channel->Register_Callback(std::bind(&Gus::DmaCallback, this, _1, _2));
#if LOG_GUS
	LOG_MSG("GUS: Assigned DMA1 address to %u", dma1);
#endif
}

void Gus::WriteToPort(Bitu port, Bitu val, Bitu iolen)
{
	//	LOG_MSG("GUS: Write to port %x val %x", port, val);
	switch (port - port_base) {
	case 0x200:
		mix_ctrl = static_cast<uint8_t>(val);
		should_change_irq_dma = true;
		return;
	case 0x208: adlib_command_reg = static_cast<uint8_t>(val); break;
	case 0x209:
		// TODO adlib_command_reg should be 4 for this to work
		// else it should just latch the value
		if (val & 0x80) {
			timers[0].has_expired = false;
			timers[1].has_expired = false;
			return;
		}
		timers[0].is_masked = (val & 0x40) > 0;
		timers[1].is_masked = (val & 0x20) > 0;
		if (val & 0x1) {
			if (!timers[0].is_counting_down) {
				PIC_AddEvent(GUS_TimerEvent, timers[0].delay, 0);
				timers[0].is_counting_down = true;
			}
		} else
			timers[0].is_counting_down = false;
		if (val & 0x2) {
			if (!timers[1].is_counting_down) {
				PIC_AddEvent(GUS_TimerEvent, timers[1].delay, 1);
				timers[1].is_counting_down = true;
			}
		} else
			timers[1].is_counting_down = false;
		break;
		// TODO Check if 0x20a register is also available on the gus
		// like on the interwave
	case 0x20b:
		if (!should_change_irq_dma)
			break;
		should_change_irq_dma = false;
		if (mix_ctrl & 0x40) {
			// IRQ configuration, only use low bits for irq 1
			const auto i = val & 0x7;
			assert(i < irq_addresses.size());
			if (irq_addresses[i])
				irq1 = irq_addresses[i];
#if LOG_GUS
			LOG_MSG("GUS: Assigned IRQ1 to %d", irq1);
#endif
		} else {
			// DMA configuration, only use low bits for dma 1
			const uint8_t i = val & 0x7;
			if (i < dma_addresses.size() && dma_addresses[i])
				UpdateDmaAddress(dma_addresses[i]);
		}
		break;
	case 0x302:
		voice_index = val & 31;
		assert(voice_index < voices.size());
		voice = voices[voice_index].get();
		break;
	case 0x303:
		selected_register = static_cast<uint8_t>(val);
		register_data = 0;
		break;
	case 0x304:
		if (iolen == 2) {
			register_data = static_cast<uint16_t>(val);
			WriteToRegister();
		} else
			register_data = static_cast<uint16_t>(val);
		break;
	case 0x305:
		register_data = static_cast<uint16_t>((0x00ff & register_data) |
		                                      val << 8);
		WriteToRegister();
		break;
	case 0x307:
		if (dram_addr < RAM_SIZE)
			ram[dram_addr] = static_cast<uint8_t>(val);
		break;
	default:
#if LOG_GUS
		LOG_MSG("GUS: Write to port 0x%x with value %x", port, val);
#endif
		break;
	}
}

void Gus::UpdatePeakAmplitudes(const float *stream)
{
	for (int i = 0; i < BUFFER_SAMPLES - 1; i += 2) {
		peak.left = std::max(peak.left, fabsf(stream[i]));
		peak.right = std::max(peak.right, fabsf(stream[i + 1]));
	}
}

void Gus::UpdateWaveLsw(int32_t &addr) const
{
	constexpr uint32_t WAVE_LSW_MASK = ~((1 << 16) - 1); // Lower wave mask
	const auto lower = static_cast<unsigned>(addr) & WAVE_LSW_MASK;
	addr = static_cast<int32_t>(lower | register_data);
}

void Gus::UpdateWaveMsw(int32_t &addr) const
{
	constexpr uint32_t WAVE_MSW_MASK = (1 << 16) - 1; // Upper wave mask
	const uint32_t upper = register_data & 0x1fff;
	const auto lower = static_cast<unsigned>(addr) & WAVE_MSW_MASK;
	addr = static_cast<int32_t>(lower | (upper << 16));
}

void Gus::WriteToRegister()
{
	// Registers that write to the general DSP
	switch (selected_register) {
	case 0xE: // Set active voice register
		selected_register = register_data >> 8; // Jazz Jackrabbit needs this
		{
			uint8_t num_voices = 1 + ((register_data >> 8) & 31);
			ActivateVoices(num_voices);
		}
		return;
	case 0x10: // Undocumented register used in Fast Tracker 2
		return;
	case 0x41: // Dma control register
		dma_ctrl = register_data >> 8;
		if (dma_ctrl & 1)
			StartDmaTransfers();
		break;

	case 0x42: // Gravis DRAM DMA address register
		dma_addr = register_data;
		return;
	case 0x43: // MSB Peek/poke DRAM position
		dram_addr = (0xff0000 & dram_addr) |
		            (static_cast<uint32_t>(register_data));
		return;
	case 0x44: // LSW Peek/poke DRAM position
		dram_addr = (0xffff & dram_addr) |
		            (static_cast<uint32_t>(register_data >> 8)) << 16;
		return;
	case 0x45: // Timer control register.  Identical in operation to Adlib's
		timer_ctrl = static_cast<uint8_t>(register_data >> 8);
		timers[0].should_raise_irq = (timer_ctrl & 0x04) > 0;
		if (!timers[0].should_raise_irq)
			irq_status &= ~0x04;
		timers[1].should_raise_irq = (timer_ctrl & 0x08) > 0;
		if (!timers[1].should_raise_irq)
			irq_status &= ~0x08;
		return;
	case 0x46: // Timer 1 control
		timers[0].value = static_cast<uint8_t>(register_data >> 8);
		timers[0].delay = (0x100 - timers[0].value) * TIMER_1_DEFAULT_DELAY;
		return;
	case 0x47: // Timer 2 control
		timers[1].value = static_cast<uint8_t>(register_data >> 8);
		timers[1].delay = (0x100 - timers[1].value) * TIMER_2_DEFAULT_DELAY;
		return;
	case 0x49: // DMA sampling control register
		sample_ctrl = static_cast<uint8_t>(register_data >> 8);
		if (sample_ctrl & 1)
			StartDmaTransfers();
		return;
	case 0x4c: // Runtime control
		irq_enabled = register_data & 0x4;
		{
			const auto state = (register_data >> 8) & 7;
			if (state == 0)
				StopPlayback();
			else if (state == 1)
				PrepareForPlayback();
			else if (active_voices)
				BeginPlayback();
		}
		CheckIrq();
		return;
	}

	// All the registers below here involve voices
	if (!voice)
		return;

	uint8_t data;
	// Registers that write to the current voice
	switch (selected_register) {
	case 0x0: // Voice wave control register
		if (voice->UpdateWaveState(register_data >> 8))
			CheckVoiceIrq();
		break;
	case 0x1: // Voice rate control register
		voice->WriteWaveRate(register_data);
		break;
	case 0x2: // Voice MSW start address register
		UpdateWaveMsw(voice->wave_ctrl.start);
		break;
	case 0x3: // Voice LSW start address register
		UpdateWaveLsw(voice->wave_ctrl.start);
		break;
	case 0x4: // Voice MSW end address register
		UpdateWaveMsw(voice->wave_ctrl.end);
		break;
	case 0x5: // Voice LSW end address register
		UpdateWaveLsw(voice->wave_ctrl.end);
		break;
	case 0x6: // Voice volume rate register
		voice->WriteVolRate(register_data >> 8);
		break;
	case 0x7: // Voice volume start register  EEEEMMMM
		data = register_data >> 8;
		// Don't need to bounds-check the value because it's implied:
		// 'data' is a uint8, so is 255 at most. 255 << 4 = 4080, which
		// falls within-bounds of the 4096-long vol_scalars array.
		voice->vol_ctrl.start = (data << 4) * VOLUME_INC_SCALAR;
		break;
	case 0x8: // Voice volume end register  EEEEMMMM
		data = register_data >> 8;
		// Same as above regarding bound-checking.
		voice->vol_ctrl.end = (data << 4) * VOLUME_INC_SCALAR;
		break;
	case 0x9: // Voice current volume register
		// Don't need to bounds-check the value because it's implied:
		// reg data is a uint16, and 65535 >> 4 takes it down to 4095,
		// which is the last element in the 4096-long vol_scalars array.
		voice->vol_ctrl.pos = (register_data >> 4) * VOLUME_INC_SCALAR;
		break;
	case 0xA: // Voice MSW current address register
		UpdateWaveMsw(voice->wave_ctrl.pos);
		break;
	case 0xB: // Voice LSW current address register
		UpdateWaveLsw(voice->wave_ctrl.pos);
		break;
	case 0xC: // Voice pan pot register
		voice->WritePanPot(register_data >> 8);
		break;
	case 0xD: // Voice volume control register
		if (voice->UpdateVolState(register_data >> 8))
			CheckVoiceIrq();
		break;
	}

#if LOG_GUS
	LOG_MSG("GUS: Unimplemented write register %x -- %x", register_select,
	        register_data);
#endif
	return;
}

static void gus_destroy(MAYBE_UNUSED Section *sec)
{
	if (gus) {
		gus->PrintStats();
		gus.reset(nullptr);
	}
}

static void gus_init(Section *sec)
{
	assert(sec);
	Section_prop *conf = dynamic_cast<Section_prop *>(sec);
	if (!conf || !conf->Get_bool("gus"))
		return;

	// Read the GUS config settings
	const auto port = static_cast<uint16_t>(conf->Get_hex("gusbase"));
	const auto dma = clamp(static_cast<uint8_t>(conf->Get_int("gusdma")), MIN_DMA_ADDRESS, MAX_DMA_ADDRESS);
	const auto irq = clamp(static_cast<uint8_t>(conf->Get_int("gusirq")), MIN_IRQ_ADDRESS, MAX_IRQ_ADDRESS);
	const std::string ultradir = conf->Get_string("ultradir");

	// Instantiate the GUS with the settings
	gus = std::make_unique<Gus>(port, dma, irq, ultradir);
	sec->AddDestroyFunction(&gus_destroy, true);
}

void init_gus_dosbox_settings(Section_prop &secprop)
{
	constexpr auto when_idle = Property::Changeable::WhenIdle;

	auto *bool_prop = secprop.Add_bool("gus", when_idle, false);
	bool_prop->Set_help("Enable Gravis UltraSound emulation.");

	auto *hex_prop = secprop.Add_hex("gusbase", when_idle, 0x240);
	const char *bases[] = {"240", "220", "260", "280",  "2a0",
	                       "2c0", "2e0", "300", nullptr};
	hex_prop->Set_values(bases);
	hex_prop->Set_help("The IO base address of the Gravis UltraSound.");

	auto *int_prop = secprop.Add_int("gusirq", when_idle, 5);
	const char *irqs[] = {"5", "3", "7", "9", "10", "11", "12", nullptr};
	int_prop->Set_values(irqs);
	int_prop->Set_help("The IRQ number of the Gravis UltraSound.");

	int_prop = secprop.Add_int("gusdma", when_idle, 3);
	const char *dmas[] = {"3", "0", "1", "5", "6", "7", nullptr};
	int_prop->Set_values(dmas);
	int_prop->Set_help("The DMA channel of the Gravis UltraSound.");

	auto *str_prop = secprop.Add_string("ultradir", when_idle, "C:\\ULTRASND");
	str_prop->Set_help("Path to UltraSound directory. In this directory\n"
	                   "there should be a MIDI directory that contains\n"
	                   "the patch files for GUS playback. Patch sets used\n"
	                   "with Timidity should work fine.");
}

void GUS_AddConfigSection(Config *conf)
{
	assert(conf);
	Section_prop *sec = conf->AddSection_prop("gus", &gus_init);
	assert(sec);
	init_gus_dosbox_settings(*sec);
}