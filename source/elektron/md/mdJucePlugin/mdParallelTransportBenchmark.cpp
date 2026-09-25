// Host-like performance bench for the Machinedrum transport. A DAW such as
// Ableton Live does not call a plug-in from one fixed thread: each audio
// buffer goes to whichever engine thread is free, while the other engine
// threads process other tracks. This bench reproduces both conditions on the
// real processor and controller: blocks are handed round-robin to a pool of
// caller threads, and optional load threads keep other cores busy. It reports
// the wall time per second of audio and the per-block distribution, so thread
// priority, placement and wait policies can be compared offline.
//
// Usage: mdParallelTransportBenchmark [--mode serial|parallel|default] [--warmup S]
//        [--seconds S] [--callers N] [--load N] [--prio normal|high|mmcss]
//        [--profile N] [--paced 0|1] [--latency-blocks N] [--model md|mm]
// MDMM_WORKER_PRIORITY=normal|high overrides the worker's MMCSS registration.
// --paced 1 runs the blocks at real-time pace like an audio device (a late
// block is an xrun, the next one starts at the following period) with the
// host flagged realtime, and reports what a DAW CPU meter shows: the time
// inside the plug-in's process call as a share of the period.
// --latency-blocks N sets the plug-in latency; any latency renders the
// machine on its own thread (md::AsyncRender).
// --profile N samples both DSPs' program counters during the measured window
// and prints the N hottest addresses with the code there, to see where the
// emulated cycles go (signal processing, or firmware waiting on a peripheral).
// --host-profile N (Windows) samples the host threads and prints the N
// functions and source lines that take the most host CPU.
// Needs GEARMULATOR_MD_FIRMWARE_BIN, like the firmware tests; --model mm finds
// the Monomachine ROM in the same folder.

#include "mdAutomationTestSupport.h"
#include "synthLib/romLoader.h"
#include "dsp56kBase/threadtools.h"
#include "dsp56kEmu/disasm.h"
#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/memory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <exception>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

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
		int profile = 0;		// hottest DSP addresses to print, 0 = no profile
		bool paced = false;		// real-time pacing: report the DAW-meter view
		int latencyBlocks = -1;	// plug-in latency in blocks, -1 = the device default
		md::MachineModel model = md::MachineModel::Machinedrum;
		int hostProfile = 0;	// hottest host functions/lines to print, 0 = off (Windows)
	};

	// Samples the program counters of the two DSPs from its own thread. The
	// reads race with the emulation on purpose (a diagnostic, not a product
	// path): a JIT DSP updates its PC at block boundaries, so each sample names
	// the block it is running or about to run, which is enough to find the
	// loops that consume the cycles.
	class PcProfiler
	{
	public:
		PcProfiler(std::array<dsp56k::DSP*, 2> _dsps, mc68k::Mc68k* _uc) : m_dsps(_dsps), m_uc(_uc)
		{
			m_thread = std::thread([this]
			{
				auto next = Clock::now();
				while(!m_exit.load(std::memory_order_relaxed))
				{
					for(size_t i = 0; i < m_dsps.size(); ++i)
						++m_histogram[i][m_dsps[i]->getPC().toWord()];
					if(m_uc)
						++m_ucHistogram[m_uc->getPC()];
					++m_samples;
					next += std::chrono::microseconds(20);
					while(Clock::now() < next)
						std::this_thread::yield();
				}
			});
		}

		~PcProfiler() { stop(); }

		void stop()
		{
			m_exit.store(true);
			if(m_thread.joinable())
				m_thread.join();
		}

		void print(const int _top)
		{
			static const char* const names[] = {"DSP1 mixer", "DSP2 producer"};
			for(size_t i = 0; i < m_dsps.size(); ++i)
			{
				std::vector<std::pair<uint32_t, uint64_t>> hot(m_histogram[i].begin(), m_histogram[i].end());
				std::sort(hot.begin(), hot.end(), [](const auto& _a, const auto& _b) { return _a.second > _b.second; });
				std::printf("mdParallelTransportBenchmark: profile %s, %llu samples, %zu distinct addresses\n",
					names[i], static_cast<unsigned long long>(m_samples), hot.size());
				auto& dsp = *m_dsps[i];
				for(int n = 0; n < _top && n < static_cast<int>(hot.size()); ++n)
				{
					const uint32_t pc = hot[static_cast<size_t>(n)].first;
					std::printf("  %5.1f%%  p:$%06x", 100.0 * static_cast<double>(hot[static_cast<size_t>(n)].second)
						/ static_cast<double>(m_samples), pc);
					// The first few instructions of the block.
					uint32_t addr = pc;
					for(int k = 0; k < 4; ++k)
					{
						const auto op = dsp.memory().get(dsp56k::MemArea_P, addr);
						const auto opB = dsp.memory().get(dsp56k::MemArea_P, addr + 1);
						std::string text;
						const auto len = dsp.disassembler().disassemble(text, op, opB, 0, 0, addr);
						std::printf("%s %s", k ? " |" : "  ", text.c_str());
						addr += std::max(1u, len);
					}
					std::printf("\n");
				}
			}
			if(const char* const dump = std::getenv("MDMM_BENCH_DSPDUMP"))
			{
				// MDMM_BENCH_DSPDUMP=start:end disassembles a P range of both DSPs,
				// with each address's share of the samples.
				const auto begin = static_cast<uint32_t>(std::strtoul(dump, nullptr, 16));
				const char* const colon = std::strchr(dump, ':');
				const auto end = colon ? static_cast<uint32_t>(std::strtoul(colon + 1, nullptr, 16)) : begin + 0x20;
				for(size_t i = 0; i < m_dsps.size(); ++i)
				{
					auto& dsp = *m_dsps[i];
					std::printf("mdParallelTransportBenchmark: dspdump %s p:$%06x-$%06x\n", names[i], begin, end);
					for(uint32_t addr = begin; addr < end;)
					{
						const auto op = dsp.memory().get(dsp56k::MemArea_P, addr);
						const auto opB = dsp.memory().get(dsp56k::MemArea_P, addr + 1);
						std::string text;
						const auto len = std::max(1u, dsp.disassembler().disassemble(text, op, opB, 0, 0, addr));
						const auto found = m_histogram[i].find(addr);
						const double share = found == m_histogram[i].end() ? 0.0
							: 100.0 * static_cast<double>(found->second) / static_cast<double>(m_samples);
						std::printf("  %5.1f%%  p:$%06x  %06x  %s\n", share, addr, op, text.c_str());
						addr += len;
					}
				}
			}
			if(!m_uc)
				return;
			std::vector<std::pair<uint32_t, uint64_t>> hot(m_ucHistogram.begin(), m_ucHistogram.end());
			std::sort(hot.begin(), hot.end(), [](const auto& _a, const auto& _b) { return _a.second > _b.second; });
			std::printf("mdParallelTransportBenchmark: profile UC, %llu samples, %zu distinct addresses\n",
				static_cast<unsigned long long>(m_samples), hot.size());
			for(int n = 0; n < _top && n < static_cast<int>(hot.size()); ++n)
			{
				const uint32_t pc = hot[static_cast<size_t>(n)].first;
				// Raw opcode words (Musashi's disassembler reads through a global
				// instance that is not necessarily this machine).
				std::printf("  %5.1f%%  $%06x ", 100.0 * static_cast<double>(hot[static_cast<size_t>(n)].second)
					/ static_cast<double>(m_samples), pc);
				for(uint32_t w = 0; w < 4; ++w)
					std::printf(" %04x", m_uc->read16(pc + w * 2));
				std::printf("\n");
			}
			if(const char* const dump = std::getenv("MDMM_BENCH_UCDUMP"))
			{
				// MDMM_BENCH_UCDUMP=start:end dumps a code range as words.
				const auto begin = static_cast<uint32_t>(std::strtoul(dump, nullptr, 16));
				const char* const colon = std::strchr(dump, ':');
				const auto end = colon ? static_cast<uint32_t>(std::strtoul(colon + 1, nullptr, 16)) : begin + 0x40;
				for(uint32_t a = begin & ~1u; a < end; a += 16)
				{
					std::printf("  ucdump $%06x:", a);
					for(uint32_t w = 0; w < 8; ++w)
						std::printf(" %04x", m_uc->read16(a + w * 2));
					std::printf("\n");
				}
			}
		}

	private:
		std::array<dsp56k::DSP*, 2> m_dsps;
		mc68k::Mc68k* m_uc = nullptr;
		std::unordered_map<uint32_t, uint64_t> m_ucHistogram;
		std::array<std::unordered_map<uint32_t, uint64_t>, 2> m_histogram;
		uint64_t m_samples = 0;
		std::atomic<bool> m_exit{false};
		std::thread m_thread;
	};

#ifdef _WIN32
	// Host-time profile: samples the instruction pointer of every thread of
	// this process from its own thread (suspend, read the context, resume),
	// then names the samples through the debug symbols. Unlike PcProfiler,
	// which shows where the emulated processors spend emulated time, this
	// shows where the host CPU goes. Threads parked in a system call are left
	// out; JIT code has no symbols and shows as [jit].
	class HostProfiler
	{
	public:
		HostProfiler()
		{
			m_samples.reserve(8u << 20);
			m_thread = std::thread([this] { run(); });
		}

		~HostProfiler() { stop(); }

		void stop()
		{
			m_exit.store(true);
			if(m_thread.joinable())
				m_thread.join();
		}

		void print(const int _top)
		{
			const HANDLE process = GetCurrentProcess();
			SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
			SymInitialize(process, nullptr, TRUE);
			std::unordered_map<uint64_t, std::pair<std::string, std::string>> names;	// address -> function, file:line
			std::unordered_map<std::string, uint64_t> functions, lines;
			std::unordered_map<DWORD, uint64_t> threads;
			std::unordered_map<DWORD, std::unordered_map<std::string, uint64_t>> threadFunctions;
			uint64_t total = 0;
			alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
			for(const auto& sample : m_samples)
			{
				auto it = names.find(sample.rip);
				if(it == names.end())
				{
					std::string function = "[jit]", line;
					if(SymGetModuleBase64(process, sample.rip))
					{
						auto* const symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
						symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
						symbol->MaxNameLen = 511;
						DWORD64 displacement = 0;
						function = SymFromAddr(process, sample.rip, &displacement, symbol) ? symbol->Name : "[unknown]";
						IMAGEHLP_LINE64 info{};
						info.SizeOfStruct = sizeof(info);
						DWORD lineDisplacement = 0;
						if(SymGetLineFromAddr64(process, sample.rip, &lineDisplacement, &info))
						{
							std::string file = info.FileName;
							const auto slash = file.find_last_of("\\/");
							if(slash != std::string::npos)
								file = file.substr(slash + 1);
							line = file + ":" + std::to_string(info.LineNumber);
						}
					}
					it = names.emplace(sample.rip, std::make_pair(function, line)).first;
				}
				const auto& function = it->second.first;
				// A thread parked in the kernel sits on its system call stub.
				if((function.rfind("Nt", 0) == 0 || function.rfind("Zw", 0) == 0) && function.find("Wait") != std::string::npos)
					continue;
				if(function == "NtDelayExecution" || function == "ZwDelayExecution" || function == "NtRemoveIoCompletion"
					|| function == "ZwRemoveIoCompletion" || function == "NtWaitForAlertByThreadId")
					continue;
				++total;
				++threads[sample.thread];
				++functions[function];
				++threadFunctions[sample.thread][function];
				if(!it->second.second.empty())
					++lines[function + "  " + it->second.second];
			}
			SymCleanup(process);
			const auto top = [&](const std::unordered_map<std::string, uint64_t>& _map, const char* _title)
			{
				std::vector<std::pair<std::string, uint64_t>> hot(_map.begin(), _map.end());
				std::sort(hot.begin(), hot.end(), [](const auto& _a, const auto& _b) { return _a.second > _b.second; });
				std::printf("mdParallelTransportBenchmark: host profile %s, %llu running samples\n", _title,
					static_cast<unsigned long long>(total));
				for(int n = 0; n < _top && n < static_cast<int>(hot.size()); ++n)
					std::printf("  %5.1f%%  %s\n", 100.0 * static_cast<double>(hot[static_cast<size_t>(n)].second)
						/ static_cast<double>(std::max<uint64_t>(total, 1)), hot[static_cast<size_t>(n)].first.c_str());
			};
			std::printf("mdParallelTransportBenchmark: host profile threads:");
			for(const auto& [id, count] : threads)
				std::printf(" %lu=%.1f%%", id, 100.0 * static_cast<double>(count) / static_cast<double>(std::max<uint64_t>(total, 1)));
			std::printf("\n");
			top(functions, "by function");
			top(lines, "by source line");
			// The two busiest threads on their own, as shares of that thread.
			std::vector<std::pair<DWORD, uint64_t>> busy(threads.begin(), threads.end());
			std::sort(busy.begin(), busy.end(), [](const auto& _a, const auto& _b) { return _a.second > _b.second; });
			for(size_t t = 0; t < busy.size() && t < 2; ++t)
			{
				const auto& map = threadFunctions[busy[t].first];
				std::vector<std::pair<std::string, uint64_t>> hot(map.begin(), map.end());
				std::sort(hot.begin(), hot.end(), [](const auto& _a, const auto& _b) { return _a.second > _b.second; });
				std::printf("mdParallelTransportBenchmark: host profile thread %lu, %llu samples\n", busy[t].first,
					static_cast<unsigned long long>(busy[t].second));
				for(int n = 0; n < _top && n < static_cast<int>(hot.size()); ++n)
					std::printf("  %5.1f%%  %s\n", 100.0 * static_cast<double>(hot[static_cast<size_t>(n)].second)
						/ static_cast<double>(std::max<uint64_t>(busy[t].second, 1)), hot[static_cast<size_t>(n)].first.c_str());
			}
		}

	private:
		struct Sample { uint64_t rip; DWORD thread; };

		void run()
		{
			const DWORD self = GetCurrentThreadId();
			std::vector<std::pair<DWORD, HANDLE>> threads;
			auto refresh = Clock::now();
			const auto enumerate = [&]
			{
				for(auto& [id, handle] : threads)
					CloseHandle(handle);
				threads.clear();
				const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
				THREADENTRY32 entry{};
				entry.dwSize = sizeof(entry);
				for(BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry))
				{
					if(entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == self)
						continue;
					if(const HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, entry.th32ThreadID))
						threads.emplace_back(entry.th32ThreadID, handle);
				}
				CloseHandle(snapshot);
			};
			enumerate();
			while(!m_exit.load(std::memory_order_relaxed))
			{
				for(const auto& [id, handle] : threads)
				{
					// Nothing may allocate while the thread is suspended: it may
					// hold the heap lock.
					if(SuspendThread(handle) == static_cast<DWORD>(-1))
						continue;
					CONTEXT context{};
					context.ContextFlags = CONTEXT_CONTROL;
					const bool ok = GetThreadContext(handle, &context) != 0;
					ResumeThread(handle);
					if(ok && m_samples.size() < m_samples.capacity())
						m_samples.push_back({context.Rip, id});
				}
				if(Clock::now() - refresh > std::chrono::milliseconds(500))
				{
					enumerate();
					refresh = Clock::now();
				}
				std::this_thread::sleep_for(std::chrono::microseconds(500));
			}
			for(auto& [id, handle] : threads)
				CloseHandle(handle);
		}

		std::vector<Sample> m_samples;
		std::atomic<bool> m_exit{false};
		std::thread m_thread;
	};
#endif

#ifdef _WIN32
	// Prints the calling thread's stack with symbols (abort/terminate
	// diagnostics: a crash of the emulation otherwise leaves no trace).
	void printStack(const char* _what)
	{
		void* frames[62];
		const USHORT count = CaptureStackBackTrace(0, 62, frames, nullptr);
		const HANDLE process = GetCurrentProcess();
		SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
		SymInitialize(process, nullptr, TRUE);
		std::fprintf(stderr, "mdParallelTransportBenchmark: %s on thread %lu\n", _what, GetCurrentThreadId());
		alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
		for(USHORT i = 0; i < count; ++i)
		{
			auto* const symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
			symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
			symbol->MaxNameLen = 255;
			DWORD64 displacement = 0;
			const auto address = reinterpret_cast<DWORD64>(frames[i]);
			IMAGEHLP_LINE64 line{};
			line.SizeOfStruct = sizeof(line);
			DWORD lineDisplacement = 0;
			const bool hasLine = SymGetLineFromAddr64(process, address, &lineDisplacement, &line) != 0;
			std::fprintf(stderr, "  #%u %s %s:%lu\n", i,
				SymFromAddr(process, address, &displacement, symbol) ? symbol->Name : "?",
				hasLine ? line.FileName : "", hasLine ? line.LineNumber : 0);
		}
		std::fflush(stderr);
	}

	void installCrashHandlers()
	{
		std::signal(SIGABRT, [](int)
		{
			printStack("SIGABRT");
		});
		std::set_terminate([]
		{
			printStack("std::terminate");
			std::abort();
		});
	}
#endif

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
			else if(key == "--profile")
				options.profile = std::max(0, std::stoi(value));
			else if(key == "--paced")
				options.paced = std::stoi(value) != 0;
			else if(key == "--latency-blocks")
				options.latencyBlocks = std::stoi(value);
			else if(key == "--host-profile")
				options.hostProfile = std::max(0, std::stoi(value));
			else if(key == "--model")
				options.model = value == "mm" ? md::MachineModel::Monomachine : md::MachineModel::Machinedrum;
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
		// A non-zero _period paces the blocks like an audio device: a block
		// starts at the next period boundary, and one that returns after its
		// period ended is an xrun - the device does not wait, so the next block
		// starts at the following boundary instead of catching up back to back.
		std::vector<int64_t> process(const int _blocks,
			const std::chrono::nanoseconds _period = std::chrono::nanoseconds(0))
		{
			std::vector<int64_t> durations(static_cast<size_t>(_blocks));
			{
				std::unique_lock<std::mutex> lock(m_mutex);
				m_durations = &durations;
				m_remaining = _blocks;
				m_index = 0;
				m_period = _period;
				m_xruns = 0;
				m_start = Clock::now() + std::chrono::milliseconds(5);
				m_nextDeadline = m_start;
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
				const auto deadline = m_nextDeadline;
				const bool paced = m_period.count() > 0;
				lock.unlock();
				if(paced)
				{
					// Yield up to the block's start time: a sleep can overshoot
					// by a whole OS timer tick and fake an xrun.
					while(Clock::now() < deadline)
						std::this_thread::yield();
				}
				const auto start = Clock::now();
				m_harness.process(1);
				const auto end = Clock::now();
				const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
				lock.lock();
				if(paced)
				{
					m_nextDeadline = deadline + m_period;
					if(end > m_nextDeadline)
					{
						// Missed the device's next period: an xrun. Resume at
						// the first boundary still ahead.
						++m_xruns;
						while(m_nextDeadline < end)
							m_nextDeadline += m_period;
					}
				}
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
		std::chrono::nanoseconds m_period{0};
		Clock::time_point m_start;
		Clock::time_point m_nextDeadline;
		int m_xruns = 0;
	public:
		int xruns() const { return m_xruns; }
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
#ifdef _WIN32
	installCrashHandlers();
#endif
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
		// "default" leaves MDMM_TRANSPORT unset: the plug-in's own setting applies.
		if(options.mode != "default")
			setTransportMode(options.mode);

		Harness harness(options.model);
		if(!harness.hasLocalFirmware())
			return SkipReturnCode;
		if(options.latencyBlocks >= 0)
			harness.processor.setLatencyBlocks(static_cast<uint32_t>(options.latencyBlocks));
		harness.prepare();
		// A paced run stands for a DAW in playback: a realtime host, whose
		// controller work runs on the message thread and not in the audio
		// callback (the harness defaults to non-realtime for the tests).
		if(options.paced)
			harness.audioProcessor.setNonRealtime(false);

		const int blocksPerSecond = 48000 / BlockSize;
		CallerPool pool(options.callers, harness, options.priority);
		pool.process(options.warmupSeconds * blocksPerSecond);

		const auto blocks = options.seconds * blocksPerSecond;
		std::unique_ptr<PcProfiler> profiler;
		if(options.profile > 0)
		{
			harness.processor.getPlugin().withDeviceLocked([&](synthLib::Device* const _device)
			{
				if(auto* const device = dynamic_cast<md::Device*>(_device))
					profiler = std::make_unique<PcProfiler>(std::array<dsp56k::DSP*, 2>{
						&device->getHardware().getDspMixer().dsp(),
						&device->getHardware().getDspProducer().dsp()}, &device->getHardware().getUC());
			});
		}
#ifdef _WIN32
		std::unique_ptr<HostProfiler> hostProfiler;
		if(options.hostProfile > 0)
			hostProfiler = std::make_unique<HostProfiler>();
#endif
		const auto asyncStats = [&]
		{
			md::AsyncRender::Stats stats{};
			harness.processor.getPlugin().withDeviceLocked([&](synthLib::Device* const _device)
			{
				if(const auto* const device = dynamic_cast<md::Device*>(_device))
					stats = device->asyncStats();
			});
			return stats;
		};
		const auto asyncBefore = asyncStats();
		harness.processor.getPlugin().withDeviceLocked([&](synthLib::Device* const _device)
		{
			if(const auto* const device = dynamic_cast<md::Device*>(_device))
				std::printf("mdParallelTransportBenchmark: device extraLatency=%u async=%d parallel requested=%d active=%d "
					"factoryCacheReady=%d factoryInitExpected=%d restorePending=%d\n",
					device->getExtraLatencySamples(), device->isRenderingAsync() ? 1 : 0,
					device->isParallelTransportRequested() ? 1 : 0, device->isParallelTransportActive() ? 1 : 0,
					device->getHardware().isFactoryFlashCacheReady() ? 1 : 0,
					device->getHardware().isFactoryFlashInitializationExpected() ? 1 : 0,
					device->isProjectStateRestorePending() ? 1 : 0);
		});
		std::vector<int64_t> durations;
		double wall = 0.0;
		{
			Load load(options.load, options.priority);
			const auto start = Clock::now();
			durations = pool.process(blocks, options.paced
				? std::chrono::nanoseconds(static_cast<int64_t>(1e9 * BlockSize / 48000.0))
				: std::chrono::nanoseconds(0));
			wall = std::chrono::duration<double>(Clock::now() - start).count();
		}
		{
			const auto asyncAfter = asyncStats();
			const auto jobs = asyncAfter.jobs - asyncBefore.jobs;
			if(jobs)
			{
				const double period = 1e9 * BlockSize / 48000.0;
				std::printf("mdParallelTransportBenchmark: async jobs=%llu render mean=%.1f%% of period, "
					"host wait mean=%.1f%% of period, max job frames=%llu backlog mean=%.2f max=%llu render idle=%.1f%% of wall "
					"queue=%.1fus/job\n",
					static_cast<unsigned long long>(jobs),
					100.0 * static_cast<double>(asyncAfter.renderNs - asyncBefore.renderNs) / static_cast<double>(jobs) / period,
					100.0 * static_cast<double>(asyncAfter.waitNs - asyncBefore.waitNs) / static_cast<double>(blocks) / period,
					static_cast<unsigned long long>(asyncAfter.maxJobFrames),
					static_cast<double>(asyncAfter.backlogSum - asyncBefore.backlogSum) / static_cast<double>(blocks),
					static_cast<unsigned long long>(asyncAfter.backlogMax),
					100.0 * static_cast<double>(asyncAfter.idleNs - asyncBefore.idleNs) / (wall * 1e9),
					static_cast<double>(asyncAfter.queueNs - asyncBefore.queueNs) / static_cast<double>(jobs) / 1000.0);
				// Distribution of the render time per job over the measured window.
				std::vector<int64_t> times;
				harness.processor.getPlugin().withDeviceLocked([&](synthLib::Device* const _device)
				{
					const auto* const device = dynamic_cast<md::Device*>(_device);
					if(!device || !device->asyncRender())
						return;
					const auto& ring = device->asyncRender()->jobTimesNs();
					const auto count = std::min<uint64_t>(jobs, md::AsyncRender::JobTimeCount);
					for(uint64_t j = asyncAfter.jobs - count; j < asyncAfter.jobs; ++j)
						times.push_back(ring[j % md::AsyncRender::JobTimeCount]);
				});
				if(std::getenv("MDMM_BENCH_TIMELINE"))
				{
					// Timeline around the first two blocks that took more than a period.
					harness.processor.getPlugin().withDeviceLocked([&](synthLib::Device* const _device)
					{
						const auto* const device = dynamic_cast<md::Device*>(_device);
						if(!device || !device->asyncRender())
							return;
						const auto& tl = device->asyncRender()->timeline();
						int shown = 0;
						for(size_t b = 8; b < durations.size() && shown < 2; ++b)
						{
							if(static_cast<double>(durations[b]) <= period)
								continue;
							++shown;
							const uint64_t center = asyncBefore.jobs + b;
							const int64_t origin = tl[(center - 8) % md::AsyncRender::JobTimeCount].hostEntry;
							for(uint64_t j = center - 8; j <= center + 3; ++j)
							{
								const auto& t = tl[j % md::AsyncRender::JobTimeCount];
								std::printf("mdParallelTransportBenchmark: tl block %+lld host %8.1f..%8.1f  render %8.1f..%8.1f us\n",
									static_cast<long long>(j) - static_cast<long long>(center),
									static_cast<double>(t.hostEntry - origin) / 1000.0, static_cast<double>(t.hostResume - origin) / 1000.0,
									static_cast<double>(t.renderStart - origin) / 1000.0, static_cast<double>(t.renderEnd - origin) / 1000.0);
							}
						}
					});
				}
				if(!times.empty() && std::getenv("MDMM_BENCH_JOBSERIES"))
				{
					// Consecutive render times in % of the period, for spotting bursts.
					std::printf("mdParallelTransportBenchmark: series");
					for(size_t j = 0; j < std::min<size_t>(times.size(), 400); ++j)
						std::printf(" %d", static_cast<int>(100.0 * static_cast<double>(times[j]) / period));
					std::printf("\n");
				}
				if(!times.empty())
				{
					const auto over = std::count_if(times.begin(), times.end(),
						[&](const int64_t _ns) { return static_cast<double>(_ns) > period; });
					std::printf("mdParallelTransportBenchmark: render p10=%.1f%% p50=%.1f%% p90=%.1f%% p99=%.1f%% max=%.1f%% "
						"jobs>period=%lld/%zu\n",
						100.0 * static_cast<double>(percentile(times, 0.1)) / period,
						100.0 * static_cast<double>(percentile(times, 0.5)) / period,
						100.0 * static_cast<double>(percentile(times, 0.9)) / period,
						100.0 * static_cast<double>(percentile(times, 0.99)) / period,
						100.0 * static_cast<double>(*std::max_element(times.begin(), times.end())) / period,
						static_cast<long long>(over), times.size());
				}
			}
		}
		if(options.paced)
		{
			// What a DAW CPU meter shows: time inside the plug-in's process
			// call as a share of the buffer period.
			const double period = 1e9 * BlockSize / 48000.0;
			double sum = 0.0;
			for(const auto d : durations)
				sum += static_cast<double>(d);
			const auto over30 = std::count_if(durations.begin(), durations.end(),
				[&](const int64_t _ns) { return static_cast<double>(_ns) > 0.3 * period; });
			// Where the worst blocks are (a window-start artifact shows as index 0-2).
			std::printf("mdParallelTransportBenchmark: blocks over one period at");
			for(size_t b = 0; b < durations.size(); ++b)
				if(static_cast<double>(durations[b]) > period)
					std::printf(" %zu(%.0f%%)", b, 100.0 * static_cast<double>(durations[b]) / period);
			std::printf("\n");
			std::printf("mdParallelTransportBenchmark: meter mean=%.1f%% p50=%.1f%% p99=%.1f%% max=%.1f%% "
				"blocks>30%%=%lld/%zu xruns=%d latencyBlocks=%u\n",
				100.0 * sum / static_cast<double>(durations.size()) / period,
				100.0 * static_cast<double>(percentile(durations, 0.5)) / period,
				100.0 * static_cast<double>(percentile(durations, 0.99)) / period,
				100.0 * static_cast<double>(*std::max_element(durations.begin(), durations.end())) / period,
				static_cast<long long>(over30), durations.size(), pool.xruns(),
				harness.processor.getPlugin().getLatencyBlocks());
		}
		if(profiler)
		{
			profiler->stop();
			profiler->print(options.profile);
		}
#ifdef _WIN32
		if(hostProfiler)
		{
			hostProfiler->stop();
			hostProfiler->print(options.hostProfile);
		}
#endif

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
