#pragma once
#include <EASTL/fixed_map.h>
#include <libriscv/machine.hpp>
#include "syscall_helpers.hpp"
#include <cstdio>
template <int W> struct multithreading;

//#define THREADS_DEBUG 1
#ifdef THREADS_DEBUG
#define THPRINT(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define THPRINT(fmt, ...) /* fmt */
#endif

template <int W>
struct thread
{
	using address_t = riscv::address_type<W>;

	multithreading<W>& threading;
	const int tid;
	address_t my_tls;
	address_t my_stack;
	// for returning to this thread
	riscv::Registers<W> stored_regs;
	// address zeroed when exiting
	address_t clear_tid = 0;
	// the current or last blocked reason
	int block_reason = 0;

	thread(multithreading<W>&, int tid,
			address_t tls, address_t stack);
	void exit();
	void suspend();
	void suspend(address_t return_value);
	void block(int reason);
	void block(int reason, address_t return_value);
	void activate();
	void resume();
};

template <int W>
struct multithreading
{
	using address_t = riscv::address_type<W>;
	using thread_t  = thread<W>;

	thread_t* create(int flags, address_t ctid, address_t ptid,
					address_t stack, address_t tls);
	thread_t* get_thread();
	thread_t* get_thread(int tid); /* or nullptr */
	bool      suspend_and_yield();
	bool      yield_to(int tid, bool store_retval = true);
	void      erase_thread(int tid);
	void      wakeup_next();
	bool      block(int reason);
	void      unblock(int tid);
	bool      wakeup_blocked(int reason);

	multithreading(riscv::Machine<W>&);
	riscv::Machine<W>& machine;
	std::vector<thread_t*> blocked;
	std::vector<thread_t*> suspended;
	eastl::fixed_map<int, thread_t, 32> threads;
	int        thread_counter = 0;
	thread_t*  m_current = nullptr;
	thread_t   main_thread;
};

template <int W>
extern void setup_multithreading(State<W>&, riscv::Machine<W>&);

template <int W>
extern multithreading<W>* setup_native_threads(riscv::Machine<W>&);

/** Implementation **/

template <int W>
inline multithreading<W>::multithreading(riscv::Machine<W>& mach)
	: machine(mach), main_thread(*this, 0, 0x0, 0x0)
{
	main_thread.my_stack = machine.cpu.reg(riscv::RISCV::REG_SP);
	m_current = &main_thread;
}

template <int W>
inline void thread<W>::resume()
{
	THPRINT("Returning to tid=%ld tls=%p stack=%p\n",
			this->tid, (void*) this->my_tls, (void*) this->my_stack);

	threading.m_current = this;
	auto& m = threading.machine;
	// restore registers
	m.cpu.registers() = this->stored_regs;
}

template <int W>
inline void thread<W>::suspend()
{
	this->stored_regs = threading.machine.cpu.registers();
	// add to suspended (NB: can throw)
	threading.suspended.push_back(this);
}

template <int W>
inline void thread<W>::suspend(address_t return_value)
{
	this->suspend();
	// set the *future* return value for this thread
	this->stored_regs.get(riscv::RISCV::REG_ARG0) = return_value;
}

template <int W>
inline void thread<W>::block(int reason)
{
	this->stored_regs = threading.machine.cpu.registers();
	this->block_reason = reason;
	// add to blocked (NB: can throw)
	threading.blocked.push_back(this);
}

template <int W>
inline void thread<W>::block(int reason, address_t return_value)
{
	this->block(reason);
	// set the block reason as the next return value
	this->stored_regs.get(riscv::RISCV::REG_ARG0) = return_value;
}

template <int W>
inline thread<W>* multithreading<W>::get_thread()
{
	return this->m_current;
}

template <int W>
inline thread<W>* multithreading<W>::get_thread(int tid)
{
	auto it = threads.find(tid);
	if (it == threads.end()) return nullptr;
	return &it->second;
}

template <int W>
inline void multithreading<W>::wakeup_next()
{
	// resume a waiting thread
	assert(!suspended.empty());
	auto* next = suspended.front();
	suspended.erase(suspended.begin());
	// resume next thread
	next->resume();
}

template <int W>
inline thread<W>::thread(
	multithreading<W>& mt, int ttid, address_t tls, address_t stack)
	: threading(mt), tid(ttid), my_tls(tls), my_stack(stack)   {}

template <int W>
inline void thread<W>::activate()
{
	threading.m_current = this;
	auto& cpu = threading.machine.cpu;
	cpu.reg(riscv::RISCV::REG_SP) = this->my_stack;
	cpu.reg(riscv::RISCV::REG_TP) = this->my_tls;
}

template <int W>
inline void thread<W>::exit()
{
	const bool exiting_myself = (threading.get_thread() == this);
	// temporary copy of thread manager
	auto& thr  = this->threading;
	// CLONE_CHILD_CLEARTID: set userspace TID value to zero
	if (this->clear_tid) {
		THPRINT("Clearing thread value for tid=%d at 0x%X\n",
				this->tid, this->clear_tid);
		threading.machine.memory.template write<uint32_t> (this->clear_tid, 0);
	}
	// delete this thread
	threading.erase_thread(this->tid);

	if (exiting_myself)
	{
		// resume next thread in suspended list
		thr.wakeup_next();
	}
}

template <int W>
inline thread<W>* multithreading<W>::create(
			int flags, address_t ctid, address_t ptid,
			address_t stack, address_t tls)
{
	const int tid = ++this->thread_counter;
	auto it = threads.emplace(tid, thread_t{*this, tid, tls, stack});
	thread_t* thread = &it.first->second;
	static const uint32_t PARENT_SETTID  = 0x00100000; /* set the TID in the parent */
	static const uint32_t CHILD_CLEARTID = 0x00200000; /* clear the TID in the child */
	static const uint32_t CHILD_SETTID   = 0x01000000; /* set the TID in the child */

	// flag for write child TID
	if (flags & CHILD_SETTID) {
		machine.memory.template write<uint32_t> (ctid, thread->tid);
	}
	if (flags & PARENT_SETTID) {
		machine.memory.template write<uint32_t> (ptid, thread->tid);
	}
	if (flags & CHILD_CLEARTID) {
		thread->clear_tid = ctid;
	}

	return thread;
}

template <int W>
inline bool multithreading<W>::suspend_and_yield()
{
	auto* thread = get_thread();
	// don't go through the ardous yielding process when alone
	if (suspended.empty()) {
		// set the return value for sched_yield
		machine.cpu.reg(riscv::RISCV::REG_ARG0) = 0;
		return false;
	}
	// suspend current thread, and return 0 when resumed
	thread->suspend(0);
	// resume some other thread
	this->wakeup_next();
	return true;
}

template <int W>
inline bool multithreading<W>::block(int reason)
{
	auto* thread = get_thread();
	if (UNLIKELY(suspended.empty())) {
		// TODO: Stop the machine here?
		throw std::runtime_error("A blocked thread has nothing to yield to!");
	}
	// block thread, write reason to future return value
	thread->block(reason, reason);
	// resume some other thread
	this->wakeup_next();
	return true;
}

template <int W>
inline bool multithreading<W>::yield_to(int tid, bool store_retval)
{
	auto* thread = get_thread();
	auto* next   = get_thread(tid);
	if (next == nullptr) {
		if (store_retval) machine.cpu.reg(riscv::RISCV::REG_ARG0) = -1;
		return false;
	}
	if (thread == next) {
		// immediately returning back to caller
		if (store_retval) machine.cpu.reg(riscv::RISCV::REG_ARG0) = 0;
		return false;
	}
	// suspend current thread
	if (store_retval)
		thread->suspend(0);
	else
		thread->suspend();
	// remove the next thread from suspension
	for (auto it = suspended.begin(); it != suspended.end(); ++it) {
		if (*it == next) {
			suspended.erase(it);
			break;
		}
	}
	// resume next thread
	next->resume();
	return true;
}

template <int W>
inline void multithreading<W>::unblock(int tid)
{
	for (auto it = blocked.begin(); it != blocked.end(); )
	{
		if ((*it)->tid == tid)
		{
			// suspend current thread
			get_thread()->suspend(0);
			// resume this thread
			(*it)->resume();
			blocked.erase(it);
			return;
		}
		else ++it;
	}
	// given thread id was not blocked
	machine.cpu.reg(riscv::RISCV::REG_ARG0) = -1;
}
template <int W>
inline bool multithreading<W>::wakeup_blocked(int reason)
{
	for (auto it = blocked.begin(); it != blocked.end(); )
	{
		// compare against block reason
		if ((*it)->block_reason == reason)
		{
			// suspend current thread
			get_thread()->suspend(0);
			// resume this thread
			(*it)->resume();
			blocked.erase(it);
			return true;
		}
		else ++it;
	}
	// nothing to wake up
	return false;
}

template <int W>
inline void multithreading<W>::erase_thread(int tid)
{
	auto it = threads.find(tid);
	assert(it != threads.end());
	threads.erase(it);
}
