// Copyright 2024 N-GINN LLC. All rights reserved.
// Copyright (C) 2024  Gaijin Games KFT.  All rights reserved
#pragma once

#include <EASTL/span.h>
#include <EASTL/string.h>
#include <nau/dag_ioSys/dag_baseIo.h>
#include <nau/kernel/kernel_config.h>

#include "nau/debug/dag_except.h"

namespace nau::iosys
{
    //
    // Async file reader for sequental data reading; designed for parallel data loading and processing
    //
    class NAU_KERNEL_EXPORT FastSeqReader : public IBaseLoad
    {
    public:
#if _TARGET_C3

#elif _TARGET_ANDROID
        // Block size can be different for every device, but with buffer 8192 bytes, the render is more smoothly
        // also Android java classes uses this value as default (DEFAULT_BUFFER_SIZE = 8192)
        enum
        {
            BUF_CNT = 6,
            BUF_SZ = 8 << 10,
            BUF_ALL_MASK = (1 << BUF_CNT) - 1,
            BLOCK_SIZE = 8 << 10
        };
#else
        enum
        {
            BUF_CNT = 6,
            BUF_SZ = 96 << 10,
            BUF_ALL_MASK = (1 << BUF_CNT) - 1,
            BLOCK_SIZE = 32 << 10
        };
#endif
        struct Range
        {
            int start, end;
        };

    public:
        FastSeqReader();
        ~FastSeqReader()
        {
            closeData();
        }

        virtual void read(void* ptr, int size) override
        {
            if(tryRead(ptr, size) != size)
                NAU_THROW(LoadException("read error", file.pos));
        }
        virtual int tryRead(void* ptr, int size) override;
        virtual int tell() override
        {
            return file.pos;
        }
        virtual void seekto(int pos) override;
        virtual void seekrel(int ofs) override
        {
            seekto(file.pos + ofs);
        }
        virtual const char* getTargetName() override
        {
            return targetFilename.c_str();
        }
        int64_t getTargetDataSize() override
        {
            return file.size;
        }

        //! assigns async file handle to read data from
        void assignFile(void* handle, unsigned base_ofs, int size, const char* tgt_name, int min_chunk_size, int max_back_seek = 0);

        //! assigns set of ranges from where data will be read; any read outside these areas results in lockup
        void setRangesOfInterest(eastl::span<Range> r)
        {
            ranges = r;
        }

        //! resets buffers and flushes pending read-op
        void reset();

        //! wait for prebuffering done
        void waitForBuffersFull();

        static const char* resolve_real_name(const char* fname, unsigned& out_base_ofs, unsigned& out_size, bool& out_non_cached);

    protected:
        struct File
        {
            int pos;
            int size;
            void* handle;
            unsigned chunkSize;
            unsigned baseOfs;
        } file;

        struct Buf
        {
            int sa, ea;
            char* data;
            short mask, handle;
        } buf[BUF_CNT], *cBuf;

        unsigned pendMask, doneMask;
        int readAheadPos, lastSweepPos, maxBackSeek;
        eastl::span<Range> ranges;
        eastl::string targetFilename;
        int64_t curThreadId = -1;

        void waitForDone(int wait_mask);
        void placeRequests();
        void closeData();
        void checkThreadSanity();
    };

    //
    // Generic file read interface based on FastSeqReader
    //
    class NAU_KERNEL_EXPORT FastSeqReadCB : public FastSeqReader
    {
    public:
        ~FastSeqReadCB()
        {
            close();
        }

        bool open(const char* fname, int max_back_seek = 0, const char* base_path = NULL);
        void close();

        bool isOpen() const
        {
            return file.handle != NULL;
        }
        int getSize() const
        {
            return file.size;
        }

        void* getFileHandle() const
        {
            return file.handle;
        }

        eastl::string targetFilename;
    };
}  // namespace nau::iosys