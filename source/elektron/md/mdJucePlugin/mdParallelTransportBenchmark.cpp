// Host-like performance bench for the Machinedrum transport. A DAW such as
// Ableton Live does not call a plug-in from one fixed thread: each audio
// buffer goes to whichever engine thread is free, while the other engine
// threads process other tracks. This bench reproduces both conditions on the
// real processor and controller: blocks are handed round-robin to a pool of
// caller threads, and optional load threads keep other cores busy. It reports
// the wall time per second of audio and the per-block distribution, so thread
// priority, placement and wait policies can be compared offline.
//
// Usage: mdParallelTransportBenchmark [--mode serial|parallel] [--warmup S]
//        [--seconds S] [--callers N] [--load N] [--prio normal|high|mmcss]
// MDMM_WORKER_PRIORITY=normal|high overrides the worker's MMCSS registration.
// Needs GEARMULATOR_MD_FIRMWARE_BIN, like the firmware tests.

#include "mdAutomationTestSupport.h"
#include "synthLib/romLoader.h"
#include "dsp56kBase/threadtools.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
	using namespace mdAutomationTest;
	using Clock = std::chrono::steady_clock;

	struct Options
	{
		std::string mode = "parallel";
		int warmupSeconds = 15;
		int seconds = 30;
		int callers = 1;
		int load = 0;
		// Priority of the caller and load threads: "normal", "high"
		// (time-critical) or "mmcss" (the MMCSS "Pro Audio" task that host
		// engine threads such as Ableton Live's AudioCalc threads join).
		std::string priority = "normal";
	};

	// Applies an Options::priority to the calling thread; returns the MMCSS
	// token to release when the thread ends.
	void* applyPriority(const std::string& _priority)
	{
		if(_priority == "mmcss")
			return dsp56k::ThreadTools::joinProAudioTask();
		if(_priority == "high")
			dsp56k::ThreadTools::setCurrentThreadPriority(dsp56k::ThreadPriority::Highest);
		return nullptr;
	}

	Options parseOptions(const int _argc, char** _argv)
	{
		Options options;
		for(int i = 1; i + 1 < _argc; i += 2)
		{
			const std::string key = _argv[i];
			const std::string value = _argv[i + 1];
			if(key == "--mode")
				options.mode = value;
			else if(key == "--warmup")
				options.warmupSeconds = std::stoi(value);
			else if(key == "--seconds")
				options.seconds = std::stoi(value);
			else if(key == "--callers")
				options.callers = std::max(1, std::stoi(value));
			else if(key == "--load")
				options.load = std::max(0, std::stoi(value));
			else if(key == "--prio")
				options.priority = value;
			else
				throw std::runtime_error("unknown option " + key);
		}
		return options;
	}

	void setTransportMode(const std::string& _mode)
	{
#ifdef _WIN32
		_putenv_s("MDMM_TRANSPORT", _mode.c_str());
#else
		setenv("MDMM_TRANSPORT", _mode.c_str(), 1);
#endif
	}

	// Round-robin caller pool: block b is processed by thread b % N, one block
	// at a time, as a host engine hands successive buffers to free threads.
	class CallerPool
	{
	public:
		CallerPool(const int _threads, Harness& _harness, const std::string& _priority) : m_harness(_harness)
		{
			for(int i = 0; i < _threads; ++i)
				m_threads.emplace_back([this, i, _threads, _priority]
				{
					void* const task = applyPriority(_priority);
					run(i, _threads);
					dsp56k::ThreadTools::leaveProAudioTask(task);
				});
		}

		~CallerPool()
		{
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				m_exit = true;
			}
			m_cv.notify_all();
			for(auto& thread : m_threads)
				thread.join();
		}

		// Processes _blocks blocks and returns each block's wall time in ns.
		std::vector<int64_t> process(const int _blocks)
		{
			std::vector<int64_t> durations(static_cast<size_t>(_blocks));
			{
				std::unique_lock<std::mutex> lock(m_mutex);
				m_durations = &durations;
				m_remaining = _blocks;
				m_index = 0;
				m_cv.notify_all();
				m_cv.wait(lock, [this] { return m_remaining == 0; });
				m_durations = nullptr;
			}
			return durations;
		}

	private:
		void run(const int _id, const int _threads)
		{
			for(;;)
			{
				std::unique_lock<std::mutex> lock(m_mutex);
				m_cv.wait(lock, [&]
				{
					return m_exit || (m_remaining > 0 && (m_turn % static_cast<uint64_t>(_threads))
						== static_cast<uint64_t>(_id));
				});
				if(m_exit)
					return;
				lock.unlock();
				const auto start = Clock::now();
				m_harness.process(1);
				const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
				lock.lock();
				(*m_durations)[static_cast<size_t>(m_index++)] = ns;
				++m_turn;
				--m_remaining;
				lock.unlock();
				m_cv.notify_all();
			}
		}

		Harness& m_harness;
		std::vector<std::thread> m_threads;
		std::mutex m_mutex;
		std::condition_variable m_cv;
		bool m_exit = false;
		uint64_t m_turn = 0;
		int m_remaining = 0;
		int m_index = 0;
		std::vector<int64_t>* m_durations = nullptr;
	};

	// Busy threads standing in for the host's other tracks.
	class Load
	{
	public:
		Load(const int _threads, const std::string& _priority)
		{
			for(int i = 0; i < _threads; ++i)
				m_threads.emplace_back([this, i, _priority]
				{
					void* const task = applyPriority(_priority);
					uint64_t x = 0x9E3779B97F4A7C15ull + static_cast<uint64_t>(i);
					while(!m_exit.load(std::memory_order_relaxed))
					{
						for(int k = 0; k < 4096; ++k)
						{
							x ^= x << 13; x ^= x >> 7; x ^= x << 17;
						}
						m_sink.fetch_add(x & 1, std::memory_order_relaxed);
					}
					dsp56k::ThreadTools::leaveProAudioTask(task);
				});
		}

		~Load()
		{
			m_exit.store(true);
			for(auto& thread : m_threads)
				thread.join();
		}

	private:
		std::vector<std::thread> m_threads;
		std::atomic<bool> m_exit{false};
		std::atomic<uint64_t> m_sink{0};
	};

	int64_t percentile(std::vector<int64_t> _values, const double _p)
	{
		if(_values.empty())
			return 0;
		const auto index = static_cast<size_t>(_p * static_cast<double>(_values.size() - 1));
		std::nth_element(_values.begin(), _values.begin() + static_cast<std::ptrdiff_t>(index), _values.end());
		return _values[index];
	}
}

int main(const int _argc, char** _argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	const auto* const romPath = std::getenv("GEARMULATOR_MD_FIRMWARE_BIN");
	if(romPath == nullptr || !*romPath)
	{
		std::printf("mdParallelTransportBenchmark: SKIP (GEARMULATOR_MD_FIRMWARE_BIN not set)\n");
		return SkipReturnCode;
	}
	try
	{
		const auto options = parseOptions(_argc, _argv);
		juce::ScopedJuceInitialiser_GUI gui;
		synthLib::RomLoader::addSearchPath(
			juce::File(romPath).getParentDirectory().getFullPathName().toStdString());
		setTransportMode(options.mode);

		Harness harness(md::MachineModel::Machinedrum);
		if(!harness.hasLocalFirmware())
			return SkipReturnCode;
		harness.prepare();

		const int blocksPerSecond = 48000 / BlockSize;
		CallerPool pool(options.callers, harness, options.priority);
		pool.process(options.warmupSeconds * blocksPerSecond);

		const auto blocks = options.seconds * blocksPerSecond;
		std::vector<int64_t> durations;
		double wall = 0.0;
		{
			Load load(options.load, options.priority);
			const auto start = Clock::now();
			durations = pool.process(blocks);
			wall = std::chrono::duration<double>(Clock::now() - start).count();
		}

		const int64_t deadlineNs = static_cast<int64_t>(1e9 * BlockSize / 48000.0);
		const auto late = std::count_if(durations.begin(), durations.end(),
			[&](const int64_t _ns) { return _ns > deadlineNs; });
		uint64_t underflow = 0, overflow = 0;
		harness.processor.getPlugin().withDeviceLocked([&](synthLib::Device* const _device)
		{
			if(const auto* const device = dynamic_cast<md::Device*>(_device))
			{
				underflow = device->getHardware().hostAudioInputUnderflowCount();
				overflow = device->getHardware().hostAudioInputOverflowCount();
			}
		});
		std::printf("mdParallelTransportBenchmark: mode=%s prio=%s callers=%d load=%d seconds=%d wall=%.2fs "
			"realtime=%.1f%% block p50=%.3fms p99=%.3fms max=%.3fms late=%lld/%d adc=%llu/%llu\n",
			options.mode.c_str(), options.priority.c_str(),
			options.callers, options.load, options.seconds, wall,
			100.0 * wall / options.seconds,
			static_cast<double>(percentile(durations, 0.5)) / 1e6,
			static_cast<double>(percentile(durations, 0.99)) / 1e6,
			static_cast<double>(*std::max_element(durations.begin(), durations.end())) / 1e6,
			static_cast<long long>(late), blocks,
			static_cast<unsigned long long>(underflow), static_cast<unsigned long long>(overflow));
		return 0;
	}
	catch(const std::exception& _error)
	{
		std::fprintf(stderr, "mdParallelTransportBenchmark: FAIL %s\n", _error.what());
		return 1;
	}
}
