// Copyright 2024 N-GINN LLC. All rights reserved.
// Copyright (C) 2024  Gaijin Games KFT.  All rights reserved
#pragma once

#include <EASTL/vector.h>
#include <nau/dag_ioSys/dag_genIo.h>
#include <nau/debug/dag_except.h>
#include <nau/kernel/kernel_config.h>

namespace nau::iosys
{
    /// @addtogroup utility_classes
    /// @{

    /// @addtogroup serialization
    /// @{

    /// @file
    /// Serialization callbacks.

    /// Base implementation of interface class for writing to abstract output stream.
    ///
    /// Implements some useful methods, but leaves a few virtual ones for
    /// real implementation of output (write(), tellPos(), seekto(), seektoend()).
    ///
    /// SaveException is thrown on write error, so there are no error return codes for methods.
    class NAU_KERNEL_EXPORT IBaseSave : public IGenSave
    {
    public:
        IBaseSave();
        virtual ~IBaseSave();

        virtual void beginBlock();
        virtual void endBlock(unsigned block_flags_2bits = 0);
        virtual int getBlockLevel();

    protected:
        /// @cond
        struct Block
        {
            int ofs;
        };

        eastl::vector<Block> blocks;
        /// @endcond
    };

    /// Base implementation of interface class for reading from abstract input stream.
    ///
    /// Implements some useful methods, but leaves a few virtual ones for
    /// real implementation of input.
    ///
    /// LoadException is thrown on read error, so there are no error return codes for methods.
    class NAU_KERNEL_EXPORT IBaseLoad : public IGenLoad
    {
    public:
        IBaseLoad();
        virtual ~IBaseLoad();

        virtual int beginBlock(unsigned* out_block_flags = nullptr);
        virtual void endBlock();
        virtual int getBlockLength();
        virtual int getBlockRest();
        virtual int getBlockLevel();

    protected:
        /// @cond
        struct Block
        {
            int ofs, len;
        };

        eastl::vector<Block> blocks;
        /// @endcond
    };

    /// @}

    /// @}
}  // namespace nau::iosys