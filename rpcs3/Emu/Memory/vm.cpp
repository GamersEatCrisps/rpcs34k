#include "stdafx.h"
#include "vm_locking.h"
#include "vm_ptr.h"
#include "vm_ref.h"
#include "vm_reservation.h"
#include "vm_var.h"

#include "Utilities/mutex.h"
#include "Utilities/cond.h"
#include "Utilities/Thread.h"
#include "Utilities/VirtualMemory.h"
#include "Utilities/address_range.h"
#include "Emu/CPU/CPUThread.h"
#include "Emu/Cell/lv2/sys_memory.h"
#include "Emu/RSX/GSRender.h"
#include <atomic>
#include <thread>
#include <deque>

LOG_CHANNEL(vm_log, "VM");

namespace vm
{
	static u8* memory_reserve_4GiB(void* _addr, u64 size = 0x100000000)
	{
		for (u64 addr = reinterpret_cast<u64>(_addr) + 0x100000000;; addr += 0x100000000)
		{
			if (auto ptr = utils::memory_reserve(size, reinterpret_cast<void*>(addr)))
			{
				return static_cast<u8*>(ptr);
			}
		}

		// TODO: a condition to break loop
		return static_cast<u8*>(utils::memory_reserve(size));
	}

	// Emulated virtual memory
	u8* const g_base_addr = memory_reserve_4GiB(reinterpret_cast<void*>(0x2'0000'0000));

	// Unprotected virtual memory mirror
	u8* const g_sudo_addr = memory_reserve_4GiB(g_base_addr);

	// Auxiliary virtual memory for executable areas
	u8* const g_exec_addr = memory_reserve_4GiB(g_sudo_addr, 0x200000000);

	// Stats for debugging
	u8* const g_stat_addr = memory_reserve_4GiB(g_exec_addr);

	// Reservation stats
	alignas(4096) u8 g_reservations[65536 / 128 * 64]{0};

	// Shareable memory bits
	alignas(4096) atomic_t<u8> g_shareable[65536]{0};

	// Memory locations
	std::vector<std::shared_ptr<block_t>> g_locations;

	// Memory mutex core
	shared_mutex g_mutex;

	// Memory mutex acknowledgement
	thread_local atomic_t<cpu_thread*>* g_tls_locked = nullptr;

	// Currently locked cache line
	atomic_t<u64> g_addr_lock = 0;

	// Memory mutex: passive locks
	std::array<atomic_t<cpu_thread*>, g_cfg.core.ppu_threads.max> g_locks{};
	std::array<atomic_t<u64>, 6> g_range_locks{};

	static void _register_lock(cpu_thread* _cpu)
	{
		for (u32 i = 0, max = g_cfg.core.ppu_threads;;)
		{
			if (!g_locks[i] && g_locks[i].compare_and_swap_test(nullptr, _cpu))
			{
				g_tls_locked = g_locks.data() + i;
				return;
			}

			if (++i == max) i = 0;
		}
	}

	static atomic_t<u64>* _register_range_lock(const u64 lock_info)
	{
		while (true)
		{
			for (auto& lock : g_range_locks)
			{
				if (!lock && lock.compare_and_swap_test(0, lock_info))
				{
					return &lock;
				}
			}
		}
	}

	static void _lock_shareable_cache(u8 /*value*/, u32 addr /*mutable*/, u32 end /*mutable*/)
	{
		// Special value to block new range locks
		g_addr_lock = addr | u64{end - addr} << 32;

		// Convert to 64K-page numbers
		addr >>= 16;
		end >>= 16;

		// Wait for range locks to clear
		for (auto& lock : g_range_locks)
		{
			while (const u64 _lock = lock.load())
			{
				if (const u32 lock_page = static_cast<u32>(_lock) >> 16)
				{
					if (lock_page < addr || lock_page >= end)
					{
						// Ignoreable range lock
						break;
					}
				}

				_mm_pause();
			}
		}
	}

	void passive_lock(cpu_thread& cpu)
	{
		if (g_tls_locked && *g_tls_locked == &cpu) [[unlikely]]
		{
			if (cpu.state & cpu_flag::wait)
			{
				while (true)
				{
					g_mutex.lock_unlock();
					cpu.state -= cpu_flag::wait + cpu_flag::memory;

					if (g_mutex.is_lockable()) [[likely]]
					{
						return;
					}

					cpu.state += cpu_flag::wait;
				}
			}

			return;
		}

		if (cpu.state & cpu_flag::memory)
		{
			cpu.state -= cpu_flag::memory + cpu_flag::wait;
		}

		if (g_mutex.is_lockable()) [[likely]]
		{
			// Optimistic path (hope that mutex is not exclusively locked)
			_register_lock(&cpu);

			if (g_mutex.is_lockable()) [[likely]]
			{
				return;
			}

			passive_unlock(cpu);
		}

		::reader_lock lock(g_mutex);
		_register_lock(&cpu);
	}

	atomic_t<u64>* range_lock(u32 addr, u32 end)
	{
		static const auto test_addr = [](u64 target, u32 addr, u32 end) -> u64
		{
			if (const u32 target_size = static_cast<u32>(target >> 32))
			{
				// Shareable info is being modified
				const u32 target_addr = static_cast<u32>(target);

				if (addr >= target_addr + target_size || end <= target_addr)
				{
					// Outside of the locked range: proceed normally
					if (g_shareable[addr >> 16])
					{
						addr &= 0xffff;
						end = ((end - 1) & 0xffff) + 1;
					}

					return u64{end} << 32 | addr;
				}

				return 0;
			}

			if (g_shareable[target >> 16])
			{
				// Target within shareable memory range
				target &= 0xffff;
			}

			if (g_shareable[addr >> 16])
			{
				// Track shareable memory locks in 0x0..0xffff address range
				addr &= 0xffff;
				end = ((end - 1) & 0xffff) + 1;
			}

			if (addr > target || end <= target)
			{
				return u64{end} << 32 | addr;
			}

			return 0;
		};

		atomic_t<u64>* _ret;

		if (u64 _a1 = test_addr(g_addr_lock.load(), addr, end)) [[likely]]
		{
			// Optimistic path (hope that address range is not locked)
			_ret = _register_range_lock(_a1);

			if (_a1 == test_addr(g_addr_lock.load(), addr, end)) [[likely]]
			{
				return _ret;
			}

			*_ret = 0;
		}

		{
			::reader_lock lock(g_mutex);
			_ret = _register_range_lock(test_addr(UINT32_MAX, addr, end));
		}

		return _ret;
	}

	void passive_unlock(cpu_thread& cpu)
	{
		if (auto& ptr = g_tls_locked)
		{
			*ptr = nullptr;
			ptr = nullptr;

			if (cpu.state & cpu_flag::memory)
			{
				cpu.state -= cpu_flag::memory;
			}
		}
	}

	void cleanup_unlock(cpu_thread& cpu) noexcept
	{
		for (u32 i = 0, max = g_cfg.core.ppu_threads; i < max; i++)
		{
			if (g_locks[i] == &cpu)
			{
				g_locks[i].compare_and_swap_test(&cpu, nullptr);
				return;
			}
		}
	}

	void temporary_unlock(cpu_thread& cpu) noexcept
	{
		cpu.state += cpu_flag::wait;

		if (g_tls_locked && g_tls_locked->compare_and_swap_test(&cpu, nullptr))
		{
			cpu.cpu_unmem();
		}
	}

	void temporary_unlock() noexcept
	{
		if (auto cpu = get_current_cpu_thread())
		{
			temporary_unlock(*cpu);
		}
	}

	reader_lock::reader_lock()
	{
		auto cpu = get_current_cpu_thread();

		if (!cpu || !g_tls_locked || !g_tls_locked->compare_and_swap_test(cpu, nullptr))
		{
			cpu = nullptr;
		}

		g_mutex.lock_shared();

		if (cpu)
		{
			_register_lock(cpu);
			cpu->state -= cpu_flag::memory;
		}
	}

	reader_lock::~reader_lock()
	{
		if (m_upgraded)
		{
			g_mutex.unlock();
		}
		else
		{
			g_mutex.unlock_shared();
		}
	}

	void reader_lock::upgrade()
	{
		if (m_upgraded)
		{
			return;
		}

		g_mutex.lock_upgrade();
		m_upgraded = true;
	}

	writer_lock::writer_lock(u32 addr /*mutable*/)
	{
		auto cpu = get_current_cpu_thread();

		if (!cpu || !g_tls_locked || !g_tls_locked->compare_and_swap_test(cpu, nullptr))
		{
			cpu = nullptr;
		}

		g_mutex.lock();

		if (addr >= 0x10000)
		{
			for (auto lock = g_locks.cbegin(), end = lock + g_cfg.core.ppu_threads; lock != end; lock++)
			{
				if (cpu_thread* ptr = *lock)
				{
					ptr->state.test_and_set(cpu_flag::memory);
				}
			}

			g_addr_lock = addr;

			if (g_shareable[addr >> 16])
			{
				// Reservation address in shareable memory range
				addr = addr & 0xffff;
			}

			for (auto& lock : g_range_locks)
			{
				while (true)
				{
					const u64 value = lock;

					// Test beginning address
					if (static_cast<u32>(value) > addr)
					{
						break;
					}

					// Test end address
					if (static_cast<u32>(value >> 32) <= addr)
					{
						break;
					}

					_mm_pause();
				}
			}

			for (auto lock = g_locks.cbegin(), end = lock + g_cfg.core.ppu_threads; lock != end; lock++)
			{
				cpu_thread* ptr;
				while ((ptr = *lock) && !(ptr->state & cpu_flag::wait))
				{
					_mm_pause();
				}
			}
		}

		if (cpu)
		{
			_register_lock(cpu);
			cpu->state -= cpu_flag::memory;
		}
	}

	writer_lock::~writer_lock()
	{
		g_addr_lock.release(0);
		g_mutex.unlock();
	}

	void reservation_lock_internal(atomic_t<u64>& res)
	{
		for (u64 i = 0;; i++)
		{
			if (!res.bts(0)) [[likely]]
			{
				break;
			}

			if (i < 15)
			{
				busy_wait(500);
			}
			else
			{
				std::this_thread::yield();
			}
		}
	}

	// Page information
	struct memory_page
	{
		// Memory flags
		atomic_t<u8> flags;
	};

	// Memory pages
	std::array<memory_page, 0x100000000 / 4096> g_pages{};

	static void _page_map(u32 addr, u8 flags, u32 size, utils::shm* shm)
	{
		if (!size || (size | addr) % 4096 || flags & page_allocated)
		{
			fmt::throw_exception("Invalid arguments (addr=0x%x, size=0x%x)" HERE, addr, size);
		}

		for (u32 i = addr / 4096; i < addr / 4096 + size / 4096; i++)
		{
			if (g_pages[i].flags)
			{
				fmt::throw_exception("Memory already mapped (addr=0x%x, size=0x%x, flags=0x%x, current_addr=0x%x)" HERE, addr, size, flags, i * 4096);
			}
		}

		if (shm && shm->flags() != 0)
		{
			_lock_shareable_cache(1, addr, addr + size);

			for (u32 i = addr / 65536; i < addr / 65536 + size / 65536; i++)
			{
				g_shareable[i] = 1;
			}

			// Unlock
			g_addr_lock.release(0);
		}

		// Notify rsx that range has become valid
		// Note: This must be done *before* memory gets mapped while holding the vm lock, otherwise
		//       the RSX might try to invalidate memory that got unmapped and remapped
		if (const auto rsxthr = g_fxo->get<rsx::thread>())
		{
			rsxthr->on_notify_memory_mapped(addr, size);
		}

		if (!shm)
		{
			utils::memory_protect(g_base_addr + addr, size, utils::protection::rw);
		}
		else if (shm->map_critical(g_base_addr + addr) != g_base_addr + addr || shm->map_critical(g_sudo_addr + addr) != g_sudo_addr + addr)
		{
			fmt::throw_exception("Memory mapping failed - blame Windows (addr=0x%x, size=0x%x, flags=0x%x)", addr, size, flags);
		}

		if (flags & page_executable)
		{
			// TODO
			utils::memory_commit(g_exec_addr + addr * 2, size * 2);
		}

		if (g_cfg.core.ppu_debug)
		{
			utils::memory_commit(g_stat_addr + addr, size);
		}

		for (u32 i = addr / 4096; i < addr / 4096 + size / 4096; i++)
		{
			if (g_pages[i].flags.exchange(flags | page_allocated))
			{
				fmt::throw_exception("Concurrent access (addr=0x%x, size=0x%x, flags=0x%x, current_addr=0x%x)" HERE, addr, size, flags, i * 4096);
			}
		}
	}

	bool page_protect(u32 addr, u32 size, u8 flags_test, u8 flags_set, u8 flags_clear)
	{
		vm::writer_lock lock(0);

		if (!size || (size | addr) % 4096)
		{
			fmt::throw_exception("Invalid arguments (addr=0x%x, size=0x%x)" HERE, addr, size);
		}

		const u8 flags_both = flags_set & flags_clear;

		flags_test  |= page_allocated;
		flags_set   &= ~flags_both;
		flags_clear &= ~flags_both;

		for (u32 i = addr / 4096; i < addr / 4096 + size / 4096; i++)
		{
			if ((g_pages[i].flags & flags_test) != (flags_test | page_allocated))
			{
				return false;
			}
		}

		if (!flags_set && !flags_clear)
		{
			return true;
		}

		u8 start_value = 0xff;

		for (u32 start = addr / 4096, end = start + size / 4096, i = start; i < end + 1; i++)
		{
			u8 new_val = 0xff;

			if (i < end)
			{
				new_val = g_pages[i].flags;
				new_val |= flags_set;
				new_val &= ~flags_clear;

				g_pages[i].flags.release(new_val);
				new_val &= (page_readable | page_writable);
			}

			if (new_val != start_value)
			{
				if (u32 page_size = (i - start) * 4096)
				{
					const auto protection = start_value & page_writable ? utils::protection::rw : (start_value & page_readable ? utils::protection::ro : utils::protection::no);
					utils::memory_protect(g_base_addr + start * 4096, page_size, protection);
				}

				start_value = new_val;
				start = i;
			}
		}

		return true;
	}

	static u32 _page_unmap(u32 addr, u32 max_size, utils::shm* shm)
	{
		if (!max_size || (max_size | addr) % 4096)
		{
			fmt::throw_exception("Invalid arguments (addr=0x%x, max_size=0x%x)" HERE, addr, max_size);
		}

		// Determine deallocation size
		u32 size = 0;
		bool is_exec = false;

		for (u32 i = addr / 4096; i < addr / 4096 + max_size / 4096; i++)
		{
			if ((g_pages[i].flags & page_allocated) == 0)
			{
				break;
			}

			if (size == 0)
			{
				is_exec = !!(g_pages[i].flags & page_executable);
			}
			else
			{
				// Must be consistent
				verify(HERE), is_exec == !!(g_pages[i].flags & page_executable);
			}

			size += 4096;
		}

		for (u32 i = addr / 4096; i < addr / 4096 + size / 4096; i++)
		{
			if (!(g_pages[i].flags.exchange(0) & page_allocated))
			{
				fmt::throw_exception("Concurrent access (addr=0x%x, size=0x%x, current_addr=0x%x)" HERE, addr, size, i * 4096);
			}
		}

		if (g_shareable[addr >> 16])
		{
			_lock_shareable_cache(0, addr, addr + size);

			for (u32 i = addr / 65536; i < addr / 65536 + size / 65536; i++)
			{
				g_shareable[i] = 0;
			}

			// Unlock
			g_addr_lock.release(0);
		}

		// Notify rsx to invalidate range
		// Note: This must be done *before* memory gets unmapped while holding the vm lock, otherwise
		//       the RSX might try to call VirtualProtect on memory that is already unmapped
		if (const auto rsxthr = g_fxo->get<rsx::thread>())
		{
			rsxthr->on_notify_memory_unmapped(addr, size);
		}

		// Actually unmap memory
		if (!shm)
		{
			utils::memory_protect(g_base_addr + addr, size, utils::protection::no);
			std::memset(g_sudo_addr + addr, 0, size);
		}
		else
		{
			shm->unmap_critical(g_base_addr + addr);
			shm->unmap_critical(g_sudo_addr + addr);
		}

		if (is_exec)
		{
			utils::memory_decommit(g_exec_addr + addr * 2, size * 2);
		}

		if (g_cfg.core.ppu_debug)
		{
			utils::memory_decommit(g_stat_addr + addr, size);
		}

		return size;
	}

	bool check_addr(u32 addr, u32 size, u8 flags)
	{
		// Overflow checking
		if (addr + size < addr && (addr + size) != 0)
		{
			return false;
		}

		// Always check this flag
		flags |= page_allocated;

		for (u32 i = addr / 4096, max = (addr + size - 1) / 4096; i <= max; i++)
		{
			if ((g_pages[i].flags & flags) != flags) [[unlikely]]
			{
				return false;
			}
		}

		return true;
	}

	u32 alloc(u32 size, memory_location_t location, u32 align)
	{
		const auto block = get(location);

		if (!block)
		{
			fmt::throw_exception("Invalid memory location (%u)" HERE, +location);
		}

		return block->alloc(size, align);
	}

	u32 falloc(u32 addr, u32 size, memory_location_t location)
	{
		const auto block = get(location, addr);

		if (!block)
		{
			fmt::throw_exception("Invalid memory location (%u, addr=0x%x)" HERE, +location, addr);
		}

		return block->falloc(addr, size);
	}

	u32 dealloc(u32 addr, memory_location_t location)
	{
		const auto block = get(location, addr);

		if (!block)
		{
			fmt::throw_exception("Invalid memory location (%u, addr=0x%x)" HERE, +location, addr);
		}

		return block->dealloc(addr);
	}

	void dealloc_verbose_nothrow(u32 addr, memory_location_t location) noexcept
	{
		const auto block = get(location, addr);

		if (!block)
		{
			vm_log.error("vm::dealloc(): invalid memory location (%u, addr=0x%x)\n", +location, addr);
			return;
		}

		if (!block->dealloc(addr))
		{
			vm_log.error("vm::dealloc(): deallocation failed (addr=0x%x)\n", addr);
			return;
		}
	}

	bool block_t::try_alloc(u32 addr, u8 flags, u32 size, std::shared_ptr<utils::shm>&& shm)
	{
		// Check if memory area is already mapped
		for (u32 i = addr / 4096; i <= (addr + size - 1) / 4096; i++)
		{
			if (g_pages[i].flags)
			{
				return false;
			}
		}

		const u32 page_addr = addr + (this->flags & 0x10 ? 0x1000 : 0);
		const u32 page_size = size - (this->flags & 0x10 ? 0x2000 : 0);

		if (this->flags & 0x10)
		{
			// Mark overflow/underflow guard pages as allocated
			verify(HERE), !g_pages[addr / 4096].flags.exchange(page_allocated);
			verify(HERE), !g_pages[addr / 4096 + size / 4096 - 1].flags.exchange(page_allocated);
		}

		// Map "real" memory pages
		_page_map(page_addr, flags, page_size, shm.get());

		// Add entry
		m_map[addr] = std::make_pair(size, std::move(shm));

		return true;
	}

	block_t::block_t(u32 addr, u32 size, u64 flags)
		: addr(addr)
		, size(size)
		, flags(flags)
	{
		if (flags & 0x100)
		{
			// Special path for 4k-aligned pages
			m_common = std::make_shared<utils::shm>(size);
			verify(HERE), m_common->map_critical(vm::base(addr), utils::protection::no) == vm::base(addr);
			verify(HERE), m_common->map_critical(vm::get_super_ptr(addr)) == vm::get_super_ptr(addr);
		}
	}

	block_t::~block_t()
	{
		{
			vm::writer_lock lock(0);

			// Deallocate all memory
			for (auto it = m_map.begin(), end = m_map.end(); !m_common && it != end;)
			{
				const auto next = std::next(it);
				const auto size = it->second.first;
				_page_unmap(it->first, size, it->second.second.get());
				it = next;
			}

			// Special path for 4k-aligned pages
			if (m_common)
			{
				m_common->unmap_critical(vm::base(addr));
				m_common->unmap_critical(vm::get_super_ptr(addr));
			}
		}
	}

	u32 block_t::alloc(const u32 orig_size, u32 align, const std::shared_ptr<utils::shm>* src, u64 flags)
	{
		if (!src)
		{
			// Use the block's flags
			flags = this->flags;
		}

		vm::writer_lock lock(0);

		// Determine minimal alignment
		const u32 min_page_size = flags & 0x100 ? 0x1000 : 0x10000;

		// Align to minimal page size
		const u32 size = ::align(orig_size, min_page_size) + (flags & 0x10 ? 0x2000 : 0);

		// Check alignment (it's page allocation, so passing small values there is just silly)
		if (align < min_page_size || align != (0x80000000u >> std::countl_zero(align)))
		{
			fmt::throw_exception("Invalid alignment (size=0x%x, align=0x%x)" HERE, size, align);
		}

		// Return if size is invalid
		if (!orig_size || !size || orig_size > size || size > this->size)
		{
			return 0;
		}

		u8 pflags = page_readable | page_writable;

		if ((flags & SYS_MEMORY_PAGE_SIZE_64K) == SYS_MEMORY_PAGE_SIZE_64K)
		{
			pflags |= page_64k_size;
		}
		else if (!(flags & (SYS_MEMORY_PAGE_SIZE_MASK & ~SYS_MEMORY_PAGE_SIZE_1M)))
		{
			pflags |= page_1m_size;
		}

		// Create or import shared memory object
		std::shared_ptr<utils::shm> shm;

		if (m_common)
			verify(HERE), !src;
		else if (src)
			shm = *src;
		else
			shm = std::make_shared<utils::shm>(size);

		// Search for an appropriate place (unoptimized)
		for (u32 addr = ::align(this->addr, align); u64{addr} + size <= u64{this->addr} + this->size; addr += align)
		{
			if (try_alloc(addr, pflags, size, std::move(shm)))
			{
				return addr + (flags & 0x10 ? 0x1000 : 0);
			}
		}

		return 0;
	}

	u32 block_t::falloc(u32 addr, const u32 orig_size, const std::shared_ptr<utils::shm>* src, u64 flags)
	{
		if (!src)
		{
			// Use the block's flags
			flags = this->flags;
		}

		vm::writer_lock lock(0);

		// Determine minimal alignment
		const u32 min_page_size = flags & 0x100 ? 0x1000 : 0x10000;

		// Align to minimal page size
		const u32 size = ::align(orig_size, min_page_size);

		// return if addr or size is invalid
		if (!size || addr < this->addr || orig_size > size || addr + u64{size} > this->addr + u64{this->size} || flags & 0x10)
		{
			return 0;
		}

		u8 pflags = page_readable | page_writable;

		if ((flags & SYS_MEMORY_PAGE_SIZE_64K) == SYS_MEMORY_PAGE_SIZE_64K)
		{
			pflags |= page_64k_size;
		}
		else if (!(flags & (SYS_MEMORY_PAGE_SIZE_MASK & ~SYS_MEMORY_PAGE_SIZE_1M)))
		{
			pflags |= page_1m_size;
		}

		// Create or import shared memory object
		std::shared_ptr<utils::shm> shm;

		if (m_common)
			verify(HERE), !src;
		else if (src)
			shm = *src;
		else
			shm = std::make_shared<utils::shm>(size);

		if (!try_alloc(addr, pflags, size, std::move(shm)))
		{
			return 0;
		}

		return addr;
	}

	u32 block_t::dealloc(u32 addr, const std::shared_ptr<utils::shm>* src)
	{
		{
			vm::writer_lock lock(0);

			const auto found = m_map.find(addr - (flags & 0x10 ? 0x1000 : 0));

			if (found == m_map.end())
			{
				return 0;
			}

			if (src && found->second.second.get() != src->get())
			{
				return 0;
			}

			// Get allocation size
			const auto size = found->second.first - (flags & 0x10 ? 0x2000 : 0);

			if (flags & 0x10)
			{
				// Clear guard pages
				verify(HERE), g_pages[addr / 4096 - 1].flags.exchange(0) == page_allocated;
				verify(HERE), g_pages[addr / 4096 + size / 4096].flags.exchange(0) == page_allocated;
			}

			// Unmap "real" memory pages
			verify(HERE), size == _page_unmap(addr, size, found->second.second.get());

			// Remove entry
			m_map.erase(found);

			return size;
		}
	}

	std::pair<u32, std::shared_ptr<utils::shm>> block_t::get(u32 addr, u32 size)
	{
		if (addr < this->addr || addr + u64{size} > this->addr + u64{this->size})
		{
			return {addr, nullptr};
		}

		vm::reader_lock lock;

		const auto upper = m_map.upper_bound(addr);

		if (upper == m_map.begin())
		{
			return {addr, nullptr};
		}

		const auto found = std::prev(upper);

		// Exact address condition (size == 0)
		if (size == 0 && found->first != addr)
		{
			return {addr, nullptr};
		}

		// Special path
		if (m_common)
		{
			return {this->addr, m_common};
		}

		// Range check
		if (addr + u64{size} > found->first + u64{found->second.second->size()})
		{
			return {addr, nullptr};
		}

		return {found->first, found->second.second};
	}

	u32 block_t::imp_used(const vm::writer_lock&)
	{
		u32 result = 0;

		for (auto& entry : m_map)
		{
			result += entry.second.first - (flags & 0x10 ? 0x2000 : 0);
		}

		return result;
	}

	u32 block_t::used()
	{
		vm::writer_lock lock(0);

		return imp_used(lock);
	}

	static bool _test_map(u32 addr, u32 size)
	{
		const auto range = utils::address_range::start_length(addr, size);

		if (!range.valid())
		{
			return false;
		}

		for (auto& block : g_locations)
		{
			if (!block)
			{
				continue;
			}

			if (range.overlaps(utils::address_range::start_length(block->addr, block->size)))
			{
				return false;
			}
		}

		return true;
	}

	static std::shared_ptr<block_t> _find_map(u32 size, u32 align, u64 flags)
	{
		for (u32 addr = ::align<u32>(mem_user64k_base, align); addr - 1 < mem_rsx_base - 1; addr += align)
		{
			if (_test_map(addr, size))
			{
				return std::make_shared<block_t>(addr, size, flags);
			}
		}

		return nullptr;
	}

	static std::shared_ptr<block_t> _map(u32 addr, u32 size, u64 flags)
	{
		if (!size || (size | addr) % 4096)
		{
			fmt::throw_exception("Invalid arguments (addr=0x%x, size=0x%x)" HERE, addr, size);
		}

		if (!_test_map(addr, size))
		{
			return nullptr;
		}

		for (u32 i = addr / 4096; i < addr / 4096 + size / 4096; i++)
		{
			if (g_pages[i].flags)
			{
				fmt::throw_exception("Unexpected pages allocated (current_addr=0x%x)" HERE, i * 4096);
			}
		}

		auto block = std::make_shared<block_t>(addr, size, flags);

		g_locations.emplace_back(block);

		return block;
	}

	static std::shared_ptr<block_t> _get_map(memory_location_t location, u32 addr)
	{
		if (location != any)
		{
			// return selected location
			if (location < g_locations.size())
			{
				return g_locations[location];
			}

			return nullptr;
		}

		// search location by address
		for (auto& block : g_locations)
		{
			if (block && addr >= block->addr && addr <= block->addr + block->size - 1)
			{
				return block;
			}
		}

		return nullptr;
	}

	std::shared_ptr<block_t> map(u32 addr, u32 size, u64 flags)
	{
		vm::writer_lock lock(0);

		return _map(addr, size, flags);
	}

	std::shared_ptr<block_t> find_map(u32 orig_size, u32 align, u64 flags)
	{
		vm::writer_lock lock(0);

		// Align to minimal page size
		const u32 size = ::align(orig_size, 0x10000);

		// Check alignment
		if (align < 0x10000 || align != (0x80000000u >> std::countl_zero(align)))
		{
			fmt::throw_exception("Invalid alignment (size=0x%x, align=0x%x)" HERE, size, align);
		}

		// Return if size is invalid
		if (!size)
		{
			return nullptr;
		}

		auto block = _find_map(size, align, flags);

		if (block) g_locations.emplace_back(block);

		return block;
	}

	std::shared_ptr<block_t> unmap(u32 addr, bool must_be_empty)
	{
		vm::writer_lock lock(0);

		for (auto it = g_locations.begin() + memory_location_max; it != g_locations.end(); it++)
		{
			if (*it && (*it)->addr == addr)
			{
				if (must_be_empty && (*it)->flags & 0x3)
				{
					continue;
				}

				if (!must_be_empty && ((*it)->flags & 0x3) != 2)
				{
					continue;
				}

				if (must_be_empty && (it->use_count() != 1 || (*it)->imp_used(lock)))
				{
					return *it;
				}

				auto block = std::move(*it);
				g_locations.erase(it);
				return block;
			}
		}

		return nullptr;
	}

	std::shared_ptr<block_t> get(memory_location_t location, u32 addr)
	{
		vm::reader_lock lock;

		return _get_map(location, addr);
	}

	std::shared_ptr<block_t> reserve_map(memory_location_t location, u32 addr, u32 area_size, u64 flags)
	{
		vm::reader_lock lock;

		auto area = _get_map(location, addr);

		if (area)
		{
			return area;
		}

		lock.upgrade();

		// Allocation on arbitrary address
		if (location != any && location < g_locations.size())
		{
			// return selected location
			auto& loc = g_locations[location];

			if (!loc)
			{
				// Deferred allocation
				loc = _find_map(area_size, 0x10000000, flags);
			}

			return loc;
		}

		// Fixed address allocation
		area = _get_map(location, addr);

		if (area)
		{
			return area;
		}

		return _map(addr, area_size, flags);
	}

	bool try_access(u32 addr, void* ptr, u32 size, bool is_write)
	{
		vm::reader_lock lock;

		if (size == 0)
		{
			return true;
		}

		if (vm::check_addr(addr, size, is_write ? page_writable : page_readable))
		{
			void* src = vm::g_sudo_addr + addr;
			void* dst = ptr;

			if (is_write)
				std::swap(src, dst);

			if (size <= 16 && (size & (size - 1)) == 0 && (addr & (size - 1)) == 0)
			{
				if (is_write)
				{
					switch (size)
					{
					case 1: atomic_storage<u8>::release(*static_cast<u8*>(dst), *static_cast<u8*>(src)); break;
					case 2: atomic_storage<u16>::release(*static_cast<u16*>(dst), *static_cast<u16*>(src)); break;
					case 4: atomic_storage<u32>::release(*static_cast<u32*>(dst), *static_cast<u32*>(src)); break;
					case 8: atomic_storage<u64>::release(*static_cast<u64*>(dst), *static_cast<u64*>(src)); break;
					case 16: _mm_store_si128(static_cast<__m128i*>(dst), _mm_loadu_si128(static_cast<__m128i*>(src))); break;
					}

					return true;
				}
			}

			std::memcpy(dst, src, size);
			return true;
		}

		return false;
	}

	inline namespace ps3_
	{
		void init()
		{
			vm_log.notice("Guest memory bases address ranges:\n"
			"vm::g_base_addr = %p - %p\n"
			"vm::g_sudo_addr = %p - %p\n"
			"vm::g_exec_addr = %p - %p\n"
			"vm::g_stat_addr = %p - %p\n"
			"vm::g_reservations = %p - %p\n",
			g_base_addr, g_base_addr + UINT32_MAX,
			g_sudo_addr, g_sudo_addr + UINT32_MAX,
			g_exec_addr, g_exec_addr + 0x200000000 - 1,
			g_stat_addr, g_stat_addr + UINT32_MAX,
			g_reservations, g_reservations + sizeof(g_reservations) - 1);

			g_locations =
			{
				std::make_shared<block_t>(0x00010000, 0x1FFF0000, 0x200), // main (TEXT_SEGMENT_BASE_ADDR)
			    std::make_shared<block_t>(mem_user64k_base, mem_user64k_size, 0x201), // user 64k pages
				nullptr, // user 1m pages (OVERLAY_PPU_SPU_SHARED_SEGMENT_BASE_ADDR)
			    nullptr, // rsx context
				std::make_shared<block_t>(mem_rsx_base, mem_rsx_size), // video (RSX_FB_BASE_ADDR)
			    std::make_shared<block_t>(mem_stack_base, mem_stack_size, 0x111), // stack
				std::make_shared<block_t>(0xE0000000, 0x20000000), // SPU reserved (RAW_SPU_BASE_ADDR)
			};

			std::memset(g_reservations, 0, sizeof(g_reservations));
			std::memset(g_shareable, 0, sizeof(g_shareable));
		}
	}

	void close()
	{
		g_locations.clear();

		utils::memory_decommit(g_base_addr, 0x100000000);
		utils::memory_decommit(g_exec_addr, 0x100000000);
		utils::memory_decommit(g_stat_addr, 0x100000000);
	}
}

void fmt_class_string<vm::_ptr_base<const void, u32>>::format(std::string& out, u64 arg)
{
	fmt_class_string<u32>::format(out, arg);
}

void fmt_class_string<vm::_ptr_base<const char, u32>>::format(std::string& out, u64 arg)
{
	// Special case (may be allowed for some arguments)
	if (arg == 0)
	{
		out += reinterpret_cast<const char*>(u8"«NULL»");
		return;
	}

	// Filter certainly invalid addresses (TODO)
	if (arg < 0x10000 || arg >= 0xf0000000)
	{
		out += reinterpret_cast<const char*>(u8"«INVALID_ADDRESS:");
		fmt_class_string<u32>::format(out, arg);
		out += reinterpret_cast<const char*>(u8"»");
		return;
	}

	const auto start = out.size();

	out += reinterpret_cast<const char*>(u8"“");

	for (vm::_ptr_base<const volatile char, u32> ptr = vm::cast(arg);; ptr++)
	{
		if (!vm::check_addr(ptr.addr()))
		{
			// TODO: optimize checks
			out.resize(start);
			out += reinterpret_cast<const char*>(u8"«INVALID_ADDRESS:");
			fmt_class_string<u32>::format(out, arg);
			out += reinterpret_cast<const char*>(u8"»");
			return;
		}

		if (const char ch = *ptr)
		{
			out += ch;
		}
		else
		{
			break;
		}
	}

	out += reinterpret_cast<const char*>(u8"”");
}
