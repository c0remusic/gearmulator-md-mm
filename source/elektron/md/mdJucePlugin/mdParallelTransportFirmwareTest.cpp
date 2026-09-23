// Drives the real plug-in processor and controller on the pinned MD firmware
// with the threaded transport enabled (MDMM_TRANSPORT=parallel), so the
// startup traffic the controller generates once the firmware is MIDI ready
// (status requests, Kit/Global dump requests with wall-clock retries,
// periodic polling) runs against the producer worker exactly as it does in
// a host. A block that takes longer than a couple of seconds of wall time
// is a transport stall and fails the test with the hardware's telemetry.

#include "mdAutomationTestSupport.h"
#include "synthLib/romLoader.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace
{
	using namespace mdAutomationTest;

	void setTransportMode(const char* const _mode)
	{
#ifdef _WIN32
		_putenv_s("MDMM_TRANSPORT", _mode);
#else
		setenv("MDMM_TRANSPORT", _mode, 1);
#endif
	}

	std::string telemetry(Harness& _harness)
	{
		std::ostringstream result;
		_harness.processor.getPlugin().withDeviceLocked(
			[&result](synthLib::Device* const _device)
			{
				const auto* const device = dynamic_cast<md::Device*>(_device);
				if(device == nullptr)
					return;
				const auto& hardware = device->getHardware();
				using Site = md::Hardware::TransportWaitSite;
				result << "underflow=" << hardware.hostAudioInputUnderflowCount(0)
					<< "/" << hardware.hostAudioInputUnderflowCount(1)
					<< " overflow=" << hardware.hostAudioInputOverflowCount(0)
					<< "/" << hardware.hostAudioInputOverflowCount(1)
					<< " expiredWaits dsp=" << hardware.transportWaitClamps(Site::DspTime)
					<< " link=" << hardware.transportWaitClamps(Site::LinkProducer)
					<< " room=" << hardware.transportWaitClamps(Site::HostToDspRoom)
					<< " parked=" << hardware.transportWaitClamps(Site::ProducerParked)
					<< " mixerGate=" << hardware.transportWaitClamps(Site::MixerGate);
			});
		return result.str();
	}

	// Returns the wall time in seconds, or throws on a stall.
	double drive(const char* const _mode, const int _seconds)
	{
		setTransportMode(_mode);
		std::printf("mdParallelTransportFirmwareTest: %s creating processor\n", _mode);
		Harness harness(md::MachineModel::Machinedrum);
		std::printf("mdParallelTransportFirmwareTest: %s processor created\n", _mode);
		if(!harness.hasLocalFirmware())
		{
			allowMissingFirmware("mdParallelTransportFirmwareTest", md::MachineModel::Machinedrum);
			return -1.0;
		}
		harness.prepare();
		std::printf("mdParallelTransportFirmwareTest: %s prepared\n", _mode);
		const int blocks = _seconds * 48000 / BlockSize;
		const auto start = std::chrono::steady_clock::now();
		bool ready = false;
		bool synchronized = false;
		// A stall inside processBlock never returns to this loop: a guard
		// thread turns it into a failure with the transport telemetry.
		std::atomic<int> currentBlock{-1};
		std::atomic<bool> guardExit{false};
		std::thread guard([&]
		{
			int lastSeen = -1;
			auto lastChange = std::chrono::steady_clock::now();
			while(!guardExit.load(std::memory_order_acquire))
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(200));
				const int seen = currentBlock.load(std::memory_order_acquire);
				if(seen != lastSeen)
				{
					lastSeen = seen;
					lastChange = std::chrono::steady_clock::now();
					continue;
				}
				if(std::chrono::steady_clock::now() - lastChange > std::chrono::seconds(15))
				{
					std::fprintf(stderr, "mdParallelTransportFirmwareTest: FAIL %s stalled inside block %d (%d s of audio): %s\n",
						_mode, seen, seen * BlockSize / 48000, telemetry(harness).c_str());
					std::fflush(stderr);
					std::_Exit(1);
				}
			}
		});
		struct GuardStop
		{
			std::atomic<bool>& exit; std::thread& thread;
			~GuardStop() { exit.store(true); thread.join(); }
		} guardStop{guardExit, guard};
		for(int block = 0; block < blocks; ++block)
		{
			currentBlock.store(block, std::memory_order_release);
			const auto blockStart = std::chrono::steady_clock::now();
			harness.process(1);
			const auto blockWall = std::chrono::steady_clock::now() - blockStart;
			if(blockWall > std::chrono::seconds(2))
				throw std::runtime_error(std::string(_mode) + ": transport stalled at block "
					+ std::to_string(block) + " (" + std::to_string(block * BlockSize / 48000)
					+ " s of audio): " + telemetry(harness) + "; "
					+ harness.firmwareReadiness());
			if((block % 3750) == 0)
				std::printf("mdParallelTransportFirmwareTest: %s %d s\n", _mode, block * BlockSize / 48000);
			if(!ready && firmwareMidiReady(harness))
			{
				ready = true;
				std::printf("mdParallelTransportFirmwareTest: %s firmware MIDI ready at %d s\n",
					_mode, block * BlockSize / 48000);
			}
			if(ready && !synchronized && harness.controller.isAutomationSynchronized())
			{
				synchronized = true;
				std::printf("mdParallelTransportFirmwareTest: %s controller synchronized at %d s\n",
					_mode, block * BlockSize / 48000);
			}
		}
		const auto wall = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - start).count();
		require(ready, std::string(_mode) + ": firmware never became MIDI ready");
		require(synchronized, std::string(_mode) + ": controller never synchronized with the firmware");
		std::printf("mdParallelTransportFirmwareTest: %s %d s rendered in %.1f s wall (%.0f%% realtime) %s\n",
			_mode, _seconds, wall, wall / _seconds * 100.0, telemetry(harness).c_str());
		// The ADC timelines must keep draining exactly as under the serial
		// scheduler (the firmware soak asserts the same).
		uint64_t underflow = 0, overflow = 0;
		harness.processor.getPlugin().withDeviceLocked(
			[&](synthLib::Device* const _device)
			{
				if(const auto* const device = dynamic_cast<md::Device*>(_device))
				{
					underflow = device->getHardware().hostAudioInputUnderflowCount();
					overflow = device->getHardware().hostAudioInputOverflowCount();
				}
			});
		require(underflow == 0, std::string(_mode) + ": host-input timeline underflow");
		require(overflow == 0, std::string(_mode) + ": host-input timeline overflow");
		return wall;
	}
}

int main()
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	const auto* const romPath = std::getenv("GEARMULATOR_MD_FIRMWARE_BIN");
	std::printf("mdParallelTransportFirmwareTest: start\n");
	if(romPath == nullptr || !*romPath)
	{
		std::printf("mdParallelTransportFirmwareTest: SKIP (GEARMULATOR_MD_FIRMWARE_BIN not set)\n");
		return SkipReturnCode;
	}
	try
	{
		juce::ScopedJuceInitialiser_GUI gui;
		std::printf("mdParallelTransportFirmwareTest: juce initialised\n");
		synthLib::RomLoader::addSearchPath(
			juce::File(romPath).getParentDirectory().getFullPathName().toStdString());
		std::printf("mdParallelTransportFirmwareTest: search path added\n");
		constexpr int seconds = 40;
		const auto serial = drive("serial", seconds);
		if(serial < 0.0)
			return SkipReturnCode;
		const auto parallel = drive("parallel", seconds);
		std::printf("mdParallelTransportFirmwareTest: PASS serial=%.1fs parallel=%.1fs\n",
			serial, parallel);
		return 0;
	}
	catch(const std::exception& _error)
	{
		std::fprintf(stderr, "mdParallelTransportFirmwareTest: FAIL %s\n", _error.what());
		return 1;
	}
}
