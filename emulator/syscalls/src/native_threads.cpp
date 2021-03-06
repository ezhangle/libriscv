#include <include/threads.hpp>
#include <cassert>
#include <cstdio>
using namespace riscv;
#ifndef THREADS_SYSCALL_BASE
static const int THREADS_SYSCALL_BASE = 500;
#endif
#include "threads.cpp"

template <int W>
multithreading<W>* setup_native_threads(Machine<W>& machine)
{
	auto* mt = new multithreading<W>(machine);
	machine.add_destructor_callback([mt] { delete mt; });

	// 500: microclone
	machine.install_syscall_handler(THREADS_SYSCALL_BASE+0,
	[mt] (Machine<W>& machine) {
		const uint32_t stack = (machine.template sysarg<uint32_t> (0) & ~0xF);
		const uint32_t  func = machine.template sysarg<uint32_t> (1);
		const uint32_t   tls = machine.template sysarg<uint32_t> (2);
		const uint32_t flags = machine.template sysarg<uint32_t> (3);
		auto* parent = mt->get_thread();
		auto* thread = mt->create(
			CLONE_CHILD_SETTID | flags, tls, 0x0, stack, tls);
		// suspend and store return value for parent: child TID
		parent->suspend(thread->tid);
		// activate and setup a function call
		thread->activate();
		// the cast is a work-around for a compiler bug
		machine.setup_call(func, (const uint32_t) tls);
		// preserve A0 for the new child thread
		return machine.cpu.reg(RISCV::REG_ARG0);
	});
	// exit
	machine.install_syscall_handler(THREADS_SYSCALL_BASE+1,
	[mt] (Machine<W>& machine) {
		const int status = machine.template sysarg<int> (0);
		const int tid = mt->get_thread()->tid;
		THPRINT(">>> Exit on tid=%ld, exit status = %d\n",
				tid, (int) status);
		if (tid != 0) {
			// exit thread instead
			mt->get_thread()->exit();
			// should be a new thread now
			assert(mt->get_thread()->tid != tid);
			return machine.cpu.reg(RISCV::REG_ARG0);
		}
		machine.stop();
		return (address_type<W>) status;
	});
	// sched_yield
	machine.install_syscall_handler(THREADS_SYSCALL_BASE+2,
	[mt] (Machine<W>& machine) {
		// begone!
		mt->suspend_and_yield();
		// preserve A0 for the new thread
		return machine.cpu.reg(RISCV::REG_ARG0);
	});
	// yield_to
	machine.install_syscall_handler(THREADS_SYSCALL_BASE+3,
	[mt] (Machine<W>& machine) {
		mt->yield_to(machine.template sysarg<uint32_t> (0));
		// preserve A0 for the new thread
		return machine.cpu.reg(RISCV::REG_ARG0);
	});
	// block (w/reason)
	machine.install_syscall_handler(THREADS_SYSCALL_BASE+4,
	[mt] (Machine<W>& machine) {
		// begone!
		mt->block(machine.template sysarg<int> (0));
		// preserve A0 for the new thread
		return machine.cpu.reg(RISCV::REG_ARG0);
	});
	// unblock (w/reason)
	machine.install_syscall_handler(THREADS_SYSCALL_BASE+5,
	[mt] (Machine<W>& machine) {
		if (!mt->wakeup_blocked(machine.template sysarg<int> (0)))
			return -1u;
		// preserve A0 for the new thread
		return machine.cpu.reg(RISCV::REG_ARG0);
	});
	// unblock thread
	machine.install_syscall_handler(THREADS_SYSCALL_BASE+6,
	[mt] (Machine<W>& machine) {
		mt->unblock(machine.template sysarg<int> (0));
		// preserve A0 for the new thread
		return machine.cpu.reg(RISCV::REG_ARG0);
	});

	return mt;
}

template
multithreading<4>* setup_native_threads<4>(Machine<4>& machine);
