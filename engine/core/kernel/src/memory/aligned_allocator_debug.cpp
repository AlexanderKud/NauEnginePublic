// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#include <memory>
#include "nau/utils/performance_profiling.h"
#include "nau/memory/aligned_allocator_debug.h"

namespace nau
{
    void IAlignedAllocatorDebug::fillPattern(void* aligned, AllocationInfo& info)
    {
        ptrdiff_t diff = (uint8_t*)aligned - (uint8_t*)info.m_unaligned;
        NAU_ASSERT(diff >= sizeof(Pattern));
        uint32_t* before = (uint32_t*)((uint8_t*)aligned - sizeof(Pattern));
        uint32_t* after = (uint32_t*)((uint8_t*)aligned + info.m_size);
        *before = Pattern;
        *after = Pattern;
    }

    bool IAlignedAllocatorDebug::checkPattern(const void* aligned, AllocationInfo& info)
    {
        uint32_t* before = (uint32_t*)((uint8_t*)aligned - sizeof(Pattern));
        uint32_t* after = (uint32_t*)((uint8_t*)aligned + info.m_size);
        if (*before != Pattern || *after != Pattern)
        {
            return false;
        }
        return true;
    }

    void* IAlignedAllocatorDebug::allocateAligned(size_t size, size_t alignment)
    {
        if (!isPowerOf2(alignment))
        {
            return nullptr;
        }
        size_t reserved = size + alignment + sizeof(Pattern) * 2;
        auto* unaligned = this->allocate(reserved);
        if (unaligned != nullptr)
        {
            lock_(m_lock);
            size_t space = reserved;
            void* aligned = (uint8_t*)unaligned + sizeof(Pattern);
            aligned = std::align(alignment, size, aligned, space);
            auto infoRes = m_allocations.value().emplace(aligned, AllocationInfo());
            if (infoRes.second)
            {
                auto& info = infoRes.first->second;
                info.m_unaligned = unaligned;
                info.m_size = size;
                info.m_alignment = alignment;
//TODO Tracy                TracyAllocN(unaligned, reserved, m_name.value().c_str());
                fillPattern(aligned, info);
                return aligned;
            }
        }   
        NAU_FAILURE("IAlignedAllocatorDebug::allocateAligned failed");
        return nullptr;
    }

    void IAlignedAllocatorDebug::deallocateAligned(void* ptr)
    {
        lock_(m_lock);
        auto info = getAllocationInfo(ptr);
        NAU_ASSERT(info.first != nullptr);
        if (info.first != nullptr)
        {
            if (!checkPattern(ptr, *info.first))
            {
                NAU_FAILURE("Memory overrun detected");
            }
            this->deallocate(info.first->m_unaligned);
// TODO Tracy            TracyFreeN(info.first->m_unaligned, m_name.value().c_str());
            m_allocations.value().erase(ptr);
        }
    }

    bool IAlignedAllocatorDebug::isValid(const void* ptr) const
    {
        if (!isAligned(ptr))
        {
            return false;
        }
        return checkPattern(ptr, m_allocations.value()[const_cast<void*>(ptr)]);
    }

}  // namespace nau
