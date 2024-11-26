// Copyright 2024 N-GINN LLC. All rights reserved.

// Copyright (C) 2024  Gaijin Games KFT.  All rights reserved
#pragma once

#include "nau/utils/dag_oaHashNameMap.h"
#include "nau/osApiWrappers/dag_rwLock.h"
#include "nau/threading/dag_atomic.h"

// thread safe FastNameMap (with read-write lock)
template <bool case_insensitive = false>
struct FastNameMapTS : protected dag::OAHashNameMap<case_insensitive>
{
protected:
    uint32_t namesCount = 0;
    typedef dag::OAHashNameMap<case_insensitive> BaseNameMap;
    mutable nau::hal::OSReadWriteLock lock;  // Note: non reentrant

public:
    int getNameId(const char* name, size_t name_len) const
    {
        const typename BaseNameMap::hash_t hash = BaseNameMap::string_hash(name, name_len);  // calc hash outside lock
        lockRd();
        int id = BaseNameMap::getNameId(name, name_len, hash);
        unlockRd();
        return id;
    }
    int getNameId(const char* name) const
    {
        return getNameId(name, strlen(name));
    }
    int addNameId(const char* name, size_t name_len, typename BaseNameMap::hash_t hash)  // optimized version. addNameId doesn't call for
                                                                                         // getNameId
    {
        lockRd();
        int it = -1;
        bool foundCollision = false;
        if(BaseNameMap::noCollisions())
        {
            it = BaseNameMap::hashToStringId.findOr(hash, -1);
            if(it != -1 && !BaseNameMap::string_equal(name, name_len, it))
            {
                foundCollision = true;
                it = -1;
            }
        }
        else
            it =
                BaseNameMap::hashToStringId.findOr(hash, -1, [&, this](uint32_t id)
            {
                return BaseNameMap::string_equal(name, name_len, id);
            });
        unlockRd();

        if(it == -1)
        {
            lockWr();
            if(foundCollision)
                BaseNameMap::noCollisions() = 0;
            uint32_t id = BaseNameMap::addString(name, name_len);
            BaseNameMap::hashToStringId.emplace(hash, eastl::move(id));
            interlocked_increment(namesCount);
            unlockWr();
            return (int)id;
        }
        return it;
    }
    int addNameId(const char* name, size_t name_len)
    {
        return addNameId(name, name_len, BaseNameMap::string_hash(name, name_len));
    }
    int addNameId(const char* name)
    {
        return addNameId(name, strlen(name));
    }

    // almost full copy paste of addNameId
    // it is same as getName(addNameId(name)), but saves one lock (which can be expensive, especially in case of contention)
    const char* internName(const char* name, size_t name_len, typename BaseNameMap::hash_t hash)  // optimized version. addNameId doesn't
                                                                                                  // call for getNameId
    {
        lockRd();
        int it = -1;
        bool foundCollision = false;
        if(DAGOR_LIKELY(BaseNameMap::noCollisions()))
        {
            it = BaseNameMap::hashToStringId.findOr(hash, -1);
            if(DAGOR_UNLIKELY(it != -1 && !BaseNameMap::string_equal(name, name_len, it)))
            {
                foundCollision = true;
                it = -1;
            }
        }
        else
            it =
                BaseNameMap::hashToStringId.findOr(hash, -1, [&, this](uint32_t id)
            {
                return BaseNameMap::string_equal(name, name_len, id);
            });
        if(it != -1)
            name = BaseNameMap::getName(it);
        unlockRd();

        if(it == -1)
        {
            lockWr();
            if(foundCollision)
                BaseNameMap::noCollisions() = 0;
            uint32_t id = BaseNameMap::addString(name, name_len);
            BaseNameMap::hashToStringId.emplace(hash, eastl::move(id));
            name = BaseNameMap::getName(id);
            interlocked_increment(namesCount);
            unlockWr();
            return name;
        }
        return name;
    }
    const char* internName(const char* name)
    {
        size_t name_len = strlen(name);
        return internName(name, name_len, BaseNameMap::string_hash(name, name_len));
    }

    uint32_t nameCountRelaxed() const
    {
        return interlocked_relaxed_load(namesCount);
    }
    uint32_t nameCountAcquire() const
    {
        return interlocked_acquire_load(namesCount);
    }
    uint32_t nameCount() const
    {
        return nameCountAcquire();
    }
    const char* getName(int name_id) const
    {
        lockRd();
        const char* s = BaseNameMap::getName(name_id);
        unlockRd();
        return s;
    }
    void reset(bool erase_only = false)
    {
        lockWr();
        BaseNameMap::reset(erase_only);
        interlocked_relaxed_store(namesCount, 0);
        unlockWr();
    }
    void shrink_to_fit()
    {
        lockWr();
        BaseNameMap::shrink_to_fit();
        unlockWr();
    }
    void memInfo(size_t& used, size_t& allocated)
    {
        lockRd();
        allocated = BaseNameMap::totalAllocated();
        used = BaseNameMap::totalUsed();
        unlockRd();
    }

    template <typename Cb>
    uint32_t iterate(Cb cb)
    {
        lockRd();
        uint32_t count = iterate_names((const BaseNameMap&)*this, cb);
        unlockRd();
        return count;
    }

private:
    void lockRd() const
    {
        lock.lockRead();
    }
    void unlockRd() const
    {
        lock.unlockRead();
    }
    void lockWr() const
    {
        lock.lockWrite();
    }
    void unlockWr() const
    {
        lock.unlockWrite();
    }
};
