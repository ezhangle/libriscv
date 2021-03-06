#include <include/syscall_helpers.hpp>
#include <include/native_heap.hpp>
using namespace riscv;
//#define SYSPRINT(fmt, ...) printf(fmt, ##__VA_ARGS__)
static const uint64_t ARENA_BASE = 0x40000000;

#ifndef NATIVE_SYSCALLS_BASE
#define NATIVE_SYSCALLS_BASE    1  /* They start at 1 */
#endif

template <int W>
void setup_native_heap_syscalls(Machine<W>& machine, size_t max_memory)
{
	auto* arena = new sas_alloc::Arena(ARENA_BASE, ARENA_BASE + max_memory);
	machine.add_destructor_callback([arena] { delete arena; });

	// Malloc n+0
	machine.install_syscall_handler(NATIVE_SYSCALLS_BASE+0,
	[arena] (auto& machine) -> long
	{
		const size_t len = machine.template sysarg<address_type<W>>(0);
		auto data = arena->malloc(len);
		SYSPRINT("SYSCALL malloc(%zu) = 0x%X\n", len, data);
		return data;
	});
	// Calloc n+1
	machine.install_syscall_handler(NATIVE_SYSCALLS_BASE+1,
	[arena] (auto& machine) -> long
	{
		const auto [count, size] = 
			machine.template sysargs<address_type<W>, address_type<W>> ();
		const size_t len = count * size;
		auto data = arena->malloc(len);
		SYSPRINT("SYSCALL calloc(%zu, %zu) = 0x%X\n", count, size, data);
		if (data != 0) {
			// TODO: optimize this (CoW), **can throw**
			machine.memory.memset(data, 0, len);
		}
		return data;
	});
	// Meminfo n+2
	machine.install_syscall_handler(NATIVE_SYSCALLS_BASE+2,
	[arena] (auto& machine) -> long
	{
		const auto dst = machine.template sysarg<address_type<W>>(0);
		struct Result {
			const uint32_t bf;
			const uint32_t bu;
			const uint32_t cu;
		} result = {
			.bf = (uint32_t) arena->bytes_free(),
			.bu = (uint32_t) arena->bytes_used(),
			.cu = (uint32_t) arena->chunks_used()
		};
		int ret = (dst != 0) ? 0 : -1;
		SYSPRINT("SYSCALL meminfo(0x%X) = %d\n", ptr, ret);
		if (ret == 0) {
			machine.copy_to_guest(dst, &result, sizeof(result));
		}
		return ret;
	});
	// Free n+3
	machine.install_syscall_handler(NATIVE_SYSCALLS_BASE+3,
	[arena] (auto& machine) -> long
	{
		const auto ptr = machine.template sysarg<address_type<W>>(0);
		int ret = arena->free(ptr);
		SYSPRINT("SYSCALL free(0x%X) = %d\n", ptr, ret);
		return ret;
	});
}
template <int W>
void setup_native_memory_syscalls(Machine<W>& machine, bool trusted)
{
	if (trusted == false)
	{
		// Memcpy n+4
		machine.install_syscall_handler(NATIVE_SYSCALLS_BASE+4,
		[] (auto& m) -> long
		{
			auto [dst, src, len] =
				m.template sysargs<address_type<W>, address_type<W>, address_type<W>> ();
			SYSPRINT("SYSCALL memcpy(%#X, %#X, %u)\n", dst, src, len);
			m.cpu.increment_counter(2 * len);
			if ((dst & 3) == (src & 3)) {
				while ((src & 3) != 0 && len > 0) {
					m.memory.template write<uint8_t> (dst++,
						m.memory.template read<uint8_t> (src++));
					len --;
				}
				while (len >= 16) {
					m.memory.template write<uint32_t> (dst + 0,
						m.memory.template read<uint32_t> (src + 0));
					m.memory.template write<uint32_t> (dst + 4,
						m.memory.template read<uint32_t> (src + 4));
					m.memory.template write<uint32_t> (dst + 8,
						m.memory.template read<uint32_t> (src + 8));
					m.memory.template write<uint32_t> (dst + 12,
						m.memory.template read<uint32_t> (src + 12));
					dst += 16; src += 16; len -= 16;
				}
				while (len >= 4) {
					m.memory.template write<uint32_t> (dst,
						m.memory.template read<uint32_t> (src));
					dst += 4; src += 4; len -= 4;
				}
			}
			while (len > 0) {
				m.memory.template write<uint8_t> (dst++,
					m.memory.template read<uint8_t> (src++));
				len --;
			}
			return dst;
		});
		// Memset n+5
		machine.install_syscall_handler(NATIVE_SYSCALLS_BASE+5,
		[] (auto& m) -> long
		{
			const auto [dst, value, len] = 
				m.template sysargs<address_type<W>, address_type<W>, address_type<W>> ();
			SYSPRINT("SYSCALL memset(%#X, %#X, %u)\n", dst, value, len);
			for (size_t i = 0; i < len; i++) {
				m.memory.template write<uint8_t> (dst + i, value);
			}
			m.cpu.increment_counter(len);
			return dst;
		});
	} else {
		/// trusted system calls ///
		// Memcpy n+4
		machine.install_syscall_handler(NATIVE_SYSCALLS_BASE+4,
		[] (auto& m) -> long
		{
			auto [dst, src, len] =
				m.template sysargs<address_type<W>, address_type<W>, address_type<W>> ();
			SYSPRINT("SYSCALL memcpy(%#X, %#X, %u)\n", dst, src, len);
			m.cpu.increment_counter(2 * len);
			m.memory.memview(src, len,
				[&m] (const uint8_t* data, size_t len) {
					auto dst = m.template sysarg <address_type<W>> (0);
					m.memory.memcpy(dst, data, len);
				});
			return dst;
		});
		// Memset n+5
		machine.install_syscall_handler(NATIVE_SYSCALLS_BASE+5,
		[] (auto& m) -> long
		{
			const auto [dst, value, len] = 
				m.template sysargs<address_type<W>, address_type<W>, address_type<W>> ();
			SYSPRINT("SYSCALL memset(%#X, %#X, %u)\n", dst, value, len);
			m.memory.memset(dst, value, len);
			m.cpu.increment_counter(len);
			return dst;
		});
	} // trusted

	// Memmove n+6
	machine.install_syscall_handler(NATIVE_SYSCALLS_BASE+6,
	[] (auto& m) -> long
	{
		auto [dst, src, len] = 
			m.template sysargs<address_type<W>, address_type<W>, address_type<W>> ();
		SYSPRINT("SYSCALL memmove(%#X, %#X, %u)\n", dst, src, len);
		if (src < dst)
		{
			for (unsigned i = 0; i != len; i++) {
				m.memory.template write<uint8_t> (dst + i, 
					m.memory.template read<uint8_t> (src + i));
			}
		} else {
			while (len-- != 0) {
				m.memory.template write<uint8_t> (dst + len, 
					m.memory.template read<uint8_t> (src + len));
			}
		}
		m.cpu.increment_counter(2 * len);
		return dst;
	});
	// Print backtrace n+7
	machine.install_syscall_handler(NATIVE_SYSCALLS_BASE+7,
	[] (auto& m) -> long
	{
		m.memory.print_backtrace(
			[] (const char* buffer, size_t len) {
				printf("%.*s\n", (int)len, buffer);
			});
		return 0;
	});
}

/* le sigh */
template void setup_native_heap_syscalls<4>(Machine<4>&, size_t);
template void setup_native_memory_syscalls<4>(Machine<4>&, bool);
