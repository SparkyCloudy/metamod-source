#ifndef __SH_PAGEALLOC_H__
#define __SH_PAGEALLOC_H__

# if	/********/ defined _WIN32
#		include <windows.h>
# elif /******/ defined __linux__
#		include <sys/mman.h>
#		include <unistd.h>
# else
#		error Unsupported OS/Compiler
# endif


namespace SourceHook
{

	/*
	Class which lets us allocate memory regions in special pages only meant for on the fly code generation.

	If we alloc with malloc and then set the page access type to read/exec only, other regions returned by
	malloc that are in the same page would lose their write access as well and the process could crash.

	Allocating one page per code generation session is usually a waste of memory and on some platforms also
	a waste of virtual address space (Windows� VirtualAlloc has a granularity of 64K).


	IMPORTANT: the memory that Alloc() returns is not a in a defined state!
	It could be in read+exec OR read+write mode.
	-> call SetRE() or SetRW() before using allocated memory!
	*/
	class CPageAlloc
	{
		struct AllocationUnit
		{
			size_t begin_offset;
			size_t size;

			AllocationUnit(size_t p_offs, size_t p_size) : begin_offset(p_offs), size(p_size)
			{
			}

			bool operator < (const AllocationUnit &other) const
			{
				return begin_offset < other.begin_offset;
			}
		};

		typedef List<AllocationUnit> AUList;
		struct AllocatedRegion
		{
			void *startPtr;
			size_t size;
			bool isolated;					// may contain only one AU
			size_t minAlignment;
			AUList allocUnits;

			void CheckGap(size_t gap_begin, size_t gap_end, size_t reqsize,
				size_t &smallestgap_pos, size_t &smallestgap_size, size_t &outAlignBytes)
			{
				size_t gapsize = gap_end - gap_begin;
				// How many bytes do we actually need here?
				//   = requested size + alignment bytes
				size_t neededSize = reqsize;
				size_t alignBytes = minAlignment - ((reinterpret_cast<intptr_t>(startPtr) + gap_begin) % minAlignment);

				alignBytes %= minAlignment;
				neededSize += alignBytes;

				if (gap_end - gap_begin >= neededSize)
				{
					if (gap_end - gap_begin < smallestgap_size)
					{
						smallestgap_size = gap_end - gap_begin;
						smallestgap_pos = gap_begin;
						outAlignBytes = alignBytes;
					}
				}
			}

			bool TryAlloc(size_t reqsize, void * &outAddr)
			{
				// Check for isolated
				if (isolated && !allocUnits.empty())
					return false;

				// Find the smallest gap where req fits
				size_t lastend = 0;
				size_t smallestgap_pos = size + 1;
				size_t smallestgap_size = size + 1;
				size_t alignmentbytes = 0;

				for (AUList::iterator iter = allocUnits.begin(); iter != allocUnits.end(); ++iter)
				{
					CheckGap(lastend, iter->begin_offset, reqsize, smallestgap_pos, smallestgap_size, alignmentbytes);
					lastend = iter->begin_offset + iter->size;
				}

				CheckGap(lastend, size, reqsize, smallestgap_pos, smallestgap_size, alignmentbytes);

				if (smallestgap_pos < size)
				{
					outAddr = reinterpret_cast<void*>(reinterpret_cast<char*>(startPtr) + smallestgap_pos + alignmentbytes);
					allocUnits.push_sorted( AllocationUnit(smallestgap_pos, reqsize + alignmentbytes) );
					return true;
				}
				else
				{
					return false;
				}
			}

			bool TryFree(void *addr)
			{
				if (addr < startPtr || addr >= reinterpret_cast<void*>(reinterpret_cast<char*>(startPtr) + size))
					return false;

				intptr_t start = reinterpret_cast<intptr_t>(startPtr);

				for (AUList::iterator iter = allocUnits.begin(); iter != allocUnits.end(); ++iter)
				{
					size_t AUBegin = start + iter->begin_offset;
					void *alignedAUBegin = reinterpret_cast<void*>(
						AUBegin + ((minAlignment - AUBegin % minAlignment) % minAlignment)
						);
					
					if (addr == alignedAUBegin)
					{
						allocUnits.erase(iter);
						return true;
					}
				}

				return false;
			}

			bool Contains(void *addr)
			{
				return addr >= startPtr && addr < reinterpret_cast<void*>(reinterpret_cast<char*>(startPtr) + size);
			}

			void FreeRegion()
			{
#ifdef __linux__
				munmap(startPtr, size);
#else
				VirtualFree(startPtr, 0, MEM_RELEASE);
#endif
			}
		};

		typedef List<AllocatedRegion> ARList;

		size_t m_MinAlignment;
		size_t m_PageSize;
		ARList m_Regions;

		bool AddRegion(size_t minSize, bool isolated)
		{
			AllocatedRegion newRegion;
			newRegion.startPtr = 0;
			newRegion.isolated = isolated;
			newRegion.minAlignment = m_MinAlignment;

			// Compute real size -> align up to m_PageSize boundary

			newRegion.size = minSize - (minSize % m_PageSize);
			if (newRegion.size < minSize)
				newRegion.size += m_PageSize;

#ifdef __linux__
			newRegion.startPtr = mmap(0, newRegion.size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#else
			newRegion.startPtr = VirtualAlloc(NULL, newRegion.size, MEM_COMMIT, PAGE_READWRITE);
#endif

			if (newRegion.startPtr)
			{
				m_Regions.push_back(newRegion);
				return true;
			}
			else
			{
				return false;
			}

		}

		void *AllocPriv(size_t size, bool isolated)
		{
			void *addr;

			if (!isolated)
			{
				for (ARList::iterator iter = m_Regions.begin(); iter != m_Regions.end(); ++iter)
				{
					if (iter->TryAlloc(size, addr))
						return addr;
				}
			}

			if (!AddRegion(size, isolated))
				return NULL;

			bool tmp = m_Regions.back().TryAlloc(size, addr);
			SH_ASSERT(tmp, ("TryAlloc fails after AddRegion"));
			return addr;
		}

	public:
		CPageAlloc(size_t minAlignment = 1 /* power of 2 */ ) : m_MinAlignment(minAlignment)
		{
#ifdef __linux__
			m_PageSize = sysconf(_SC_PAGESIZE);
#else
			SYSTEM_INFO sysInfo;
			GetSystemInfo(&sysInfo);
			m_PageSize = sysInfo.dwPageSize;
#endif
		}

		~CPageAlloc()
		{
			// Free all regions
			for (ARList::iterator iter = m_Regions.begin(); iter != m_Regions.end(); ++iter)
			{
				iter->FreeRegion();
			}
		}

		void *Alloc(size_t size)
		{
			return AllocPriv(size, false);
		}

		void *AllocIsolated(size_t size)
		{
			return AllocPriv(size, true);
		}

		void Free(void *ptr)
		{
			for (ARList::iterator iter = m_Regions.begin(); iter != m_Regions.end(); ++iter)
			{
				if (iter->TryFree(ptr))
				{
					if (iter->allocUnits.empty())
					{
						iter->FreeRegion();
						m_Regions.erase(iter);
					}
					break;
				}
			}
		}

		void SetRE(void *ptr)
		{
			for (ARList::iterator iter = m_Regions.begin(); iter != m_Regions.end(); ++iter)
			{
				if (iter->Contains(ptr))
				{
					SetMemAccess(iter->startPtr, iter->size, SH_MEM_READ | SH_MEM_EXEC);
					break;
				}
			}
		}

		void SetRW(void *ptr)
		{
			for (ARList::iterator iter = m_Regions.begin(); iter != m_Regions.end(); ++iter)
			{
				if (iter->Contains(ptr))
				{
					SetMemAccess(iter->startPtr, iter->size, SH_MEM_READ | SH_MEM_WRITE);
					break;
				}
			}
		}

		size_t GetPageSize()
		{
			return m_PageSize;
		}
	};
}

#endif

