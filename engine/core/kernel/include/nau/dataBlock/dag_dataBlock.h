// Copyright 2024 N-GINN LLC. All rights reserved.
// Copyright (C) 2024  Gaijin Games KFT.  All rights reserved
#pragma once

#include <EASTL/unique_ptr.h>
#include <nau/math/dag_e3dColor.h>
#include <nau/math/math.h>
#include <nau/string/string.h>
#include <nau/utils/dag_bitFlagsMask.h>
#include <nau/utils/dag_relocatable.h>
#include <nau/utils/dag_stdint.h>

#include <cstddef>

#include "EASTL/span.h"
#include "nau/memory/mem_allocator.h"

#include "nau/dag_ioSys/dag_genIo.h"

struct ZSTD_CDict_s;
struct ZSTD_DDict_s;

struct VirtualRomFsData;

namespace nau
{
    class DataBlock;
    struct DataBlockShared;
    struct DataBlockOwned;
    struct DBNameMap;


    struct RoDataBlock;

    /// @addtogroup utility_classes
    /// @{

    /// @addtogroup serialization
    /// @{

    /// @file
    /// DataBlock class for reading and writing hierarchically structured data.

    namespace dblk
    {
        enum class ReadFlag : uint8_t
        {
            ROBUST = 1,         //< robust data load (sticky flag)
            BINARY_ONLY = 2,    //< don't try to parse text files
            RESTORE_FLAGS = 4,  //< restore sticky flags state after load() call
            ALLOW_SS = 8,       //< allow simple string during load call (see DataBlock::allowSimpleString)

#if DAGOR_DBGLEVEL < 1
            ROBUST_IN_REL = ROBUST,
#else
            ROBUST_IN_REL = 0,
#endif
        };

        using ReadFlags = BitFlagsMask<ReadFlag>;
        BITMASK_DECLARE_FLAGS_OPERATORS(ReadFlag);

        template <typename Cb>
        static inline void iterate_child_blocks(const DataBlock& db, Cb cb);
        template <typename Cb>
        static inline void iterate_blocks(const DataBlock& db, Cb cb);
        template <typename Cb>
        static inline void iterate_blocks_lev(const DataBlock& db, Cb cb, int lev = 0);
        template <typename Cb>
        static inline void iterate_params(const DataBlock& db, Cb cb);
    }  // namespace dblk

    /// Class for reading and writing hierarchically structured data.
    ///
    /// DataBlock itself is a node of the tree that has a name and hosts typified
    /// named parameters and named sub-nodes.
    ///
    /// Parameter or sub-block name can be non-unique for a given DataBlock object.
    /// This is useful for enumerating data that can be repeated arbitrary number of times.
    ///
    /// For clarity, names are restricted to C indentifier rules.
    ///
    /// Actual names are stored in NameMap that is shared by all DataBlocks in the tree.
    /// Blocks and parameters use integer ids to address names in the NameMap, so
    /// there are methods that take name ids and those that take character strings.
    /// You can use name ids when you look for multiple blocks or parameters with
    /// the same name for performance gain.
    ///
    /// DataBlock tree contents can be serialized in binary or text form.
    ///
    /// Text files of this format usually have extension ".blk".
    class NAU_KERNEL_EXPORT DataBlock
    {
    public:
        static const DataBlock emptyBlock;

        static bool strongTypeChecking;   // def= false;
        static bool singleBlockChecking;  // def= false;
        static bool allowVarTypeChange;   // def= false;
        static bool fatalOnMissingFile;   // def= true;
        static bool fatalOnLoadFailed;    // def= true;
        static bool fatalOnBadVarType;    // def= true;
        static bool fatalOnMissingVar;    // def= true;

        //! when true, allows parsing 'param=val' just like 'param:t=val'
        static bool allowSimpleString;  // def= false;

        //! when true, includes are not resolved but added as 'special' string params
        static bool parseIncludesAsParams;  // def= false;
        //! when true, special commands (@override:, @delete:, etc.) are not resolved but added as params/blocks
        static bool parseOverridesNotApply;  // def= false;
        //! when true, special commands (@override:, @delete:, etc.) are fully ignored
        static bool parseOverridesIgnored;  // def= false;
        //! when true, comments are not removed but added as 'special' string params
        static bool parseCommentsAsParams;  // def= false;

        //! error reporting pipe
        struct NAU_KERNEL_EXPORT IErrorReporterPipe
        {
            virtual void reportError(const char* error_text, bool serious_err) = 0;
        };
        struct NAU_KERNEL_EXPORT InstallReporterRAII
        {
            IErrorReporterPipe* prev = nullptr;
            InstallReporterRAII(IErrorReporterPipe* rep);
            ~InstallReporterRAII();
        };

        struct NAU_KERNEL_EXPORT IFileNotify
        {
            virtual void onFileLoaded(const char* fname) = 0;
        };
        struct NAU_KERNEL_EXPORT IIncludeFileResolver
        {
            virtual bool resolveIncludeFile(nau::string& inout_fname) = 0;
        };

        //! installs custom include resolver
        static void setIncludeResolver(IIncludeFileResolver* f_resolver);
        //! installs simple root include resolver (resolves "#name.inc" as $(Root)/name.inc)
        static void setRootIncludeResolver(const char* root);

    public:
        /// Parameter types enum.
        enum ParamType : uint8_t
        {
            TYPE_NONE,
            TYPE_STRING,    ///< Text string.
            TYPE_INT,       ///< Integer.
            TYPE_REAL,      ///< #real (float).
            TYPE_POINT2,    ///< Point2.
            TYPE_POINT3,    ///< Point3.
            TYPE_POINT4,    ///< Point4.
            TYPE_IPOINT2,   ///< IPoint2.
            TYPE_IPOINT3,   ///< IPoint3.
            TYPE_BOOL,      ///< Boolean.
            TYPE_E3DCOLOR,  ///< E3DCOLOR.
            TYPE_MATRIX,    ///< TMatrix.
            TYPE_INT64,     ///< int64_t
            TYPE_COUNT
        };

        template <class T>
        struct TypeOf
        {
            static constexpr int type = TYPE_NONE;
        };
        template <class T>
        struct TypeOf<T&>
        {
            static constexpr int type = TypeOf<T>::type;
        };
        template <class T>
        struct TypeOf<T&&>
        {
            static constexpr int type = TypeOf<T>::type;
        };

        /// Default constructor, constructs empty block.
        DataBlock(nau::IMemAllocator::Ptr allocator = nullptr);

        /// Constructor that loads DataBlock tree from specified file.
        /// If you want error checking, use default constructor and load().
        explicit DataBlock(const char* filename, nau::IMemAllocator::Ptr allocator = nullptr);

        /// Destructor, destroys all sub-blocks.
        ~DataBlock();

        /// Copy constructor.
        DataBlock(const DataBlock&);
        // copy contructor with move semantic
        DataBlock(DataBlock&& b);

        // setFrom.
        DataBlock& operator=(const DataBlock& right)
        {
            setFrom(&right);
            return *this;
        }
        // assignment with move-semantic
        DataBlock& operator=(DataBlock&& right);

        // copy from read-only data block.
        DataBlock& operator=(const RoDataBlock& right)
        {
            setFrom(right);
            return *this;
        }

        bool operator==(const DataBlock& rhs) const;
        bool operator!=(const DataBlock& rhs) const
        {
            return !(*this == rhs);
        }

        // install (or reset when owner_of_shared_namemap=nullptr) shared namemap (may be reused for several BLKs)
        void setSharedNameMapAndClearData(DataBlock* owner_of_shared_namemap);

        /// Delete all sub-blocks.
        void clearData();

        /// Reset DataBlock object (clear all data & names).
        void reset();

        /// Reset DataBlock object (clear all data & names), also releases RO namemap if used any.
        void resetAndReleaseRoNameMap();

        /// @name Loading
        /// @{

        /// Load DataBlock tree from specified text.
        /// fname is used only for error reporting.
        inline bool loadText(const char* text, int text_len, const char* fname = nullptr)
        {
            return loadText(text, text_len, fname, nullptr);
        }

        /// Load DataBlock tree from arbitrary stream
        /// Data may be presented like text, binary or stream data
        /// fname is used to correctly parse include directives when loading from a text file
        inline bool loadFromStream(iosys::IGenLoad& crd, const char* fname = nullptr, unsigned hint_size = 128)
        {
            return loadFromStream(crd, fname, nullptr, hint_size);
        }

        /// Load DataBlock tree from any type of file, binary or text
        /// First function try to load file as binary, in fail case it
        /// try to load file as text
        inline bool load(const char* fname)
        {
            return load(fname, nullptr);
        }

        /// @}

        /// @name Saving
        /// @{

        /// Save this DataBlock (and its sub-tree) to the specified file (text form)
        inline bool saveToTextFile(const char* filename) const;

        /// Save this DataBlock (and its sub-tree) to the specified file (text compact form)
        inline bool saveToTextFileCompact(const char* filename) const;

        /// Save this DataBlock (and its sub-tree) to the arbitrary stream (text form)
        bool saveToTextStream(iosys::IGenSave& cwr) const;

        /// Save this DataBlock (and its sub-tree) to the arbitrary stream in compact form (text form)
        bool saveToTextStreamCompact(iosys::IGenSave& cwr) const;

        /// Save this DataBlock (and its sub-tree) to arbitrary stream (binary form)
        bool saveToStream(iosys::IGenSave& cwr) const;

        /// Print this DataBlock (and its sub-tree) to the arbitrary text stream with limitations; returns true when whole BLK is written
        inline bool printToTextStreamLimited(iosys::IGenSave& cwr, int max_out_line_num = -1, int max_level_depth = -1, int init_indent = 0) const;

        /// @}

        /// @name General methods (applicable for root block)
        /// @{

        /// Returns true if data in DataBlock are valid
        /// Data can be invalid if error occured while loading file
        bool isValid() const;  //== deprecated, to be deleted soon; use result of load*() instead

        /// Resolves filename if it was stored during BLK load
        const char* resolveFilename(bool file_only = false) const;

        /// do compact all data (useful when BLK construction is done and we need lesser memory consumption)
        void compact()
        {
            shrink();
        }
        void shrink();

        /// Returns true when this DataBlock is empty (no params and blocks exist)
        bool isEmpty() const
        {
            return (blockCount() + paramCount()) == 0;
        }

        /// returns true for root BLK that owns 'data'
        bool topMost() const;

        /// computes and returns memory consumed by full BLK tree
        size_t memUsed() const;
        /// @}

        /// @name Names
        /// @{

        /// Returns name id from NameMap, or -1 if there's no such name in the NameMap.
        int getNameId(const char* name) const;

        /// Returns name by name id, uses NameMap.
        /// Returns NULL if name id is not valid.
        const char* getName(int name_id) const;

        // add name to NameMap
        // Returns new name id from NameMap
        int addNameId(const char* name);

        /// @}

        /// @name Block Name
        /// @{

        /// Returns name id of this DataBlock.
        bool hasNoNameId() const
        {
            return getNameIdIncreased() == 0;
        }
        int getNameId() const
        {
            return getNameIdIncreased() - 1;
        }
        int getBlockNameId() const
        {
            return getNameId();
        }

        /// Returns name of this DataBlock.
        const char* getBlockName() const
        {
            return getName(getNameId());
        }
        /// Changes name of i-th parameter
        void changeBlockName(const char* name);

        /// @}

        /// @name Sub-blocks
        /// @{

        /// Returns number of sub-blocks in this DataBlock.
        /// Use for enumeration.
        uint32_t blockCount() const
        {
            return blocksCount;
        }

        int blockCountById(int name_id) const;
        int blockCountByName(const char* name) const
        {
            return blockCountById(getNameId(name));
        }

        /// Returns pointer to i-th sub-block.
        const DataBlock* getBlock(uint32_t i) const;
        DataBlock* getBlock(uint32_t i);

        /// Returns pointer to sub-block with specified name id, or NULL if not found.
        const DataBlock* getBlockByName(int name_id, int start_after = -1, bool expect_single = false) const;
        DataBlock* getBlockByName(int name_id, int start_after = -1, bool expect_single = false);

        const DataBlock* getBlockByNameId(int name_id) const
        {
            return getBlockByName(name_id);
        }
        DataBlock* getBlockByNameId(int name_id)
        {
            return getBlockByName(name_id);
        }

        /// Finds block by name id
        int findBlock(int name_id, int start_after = -1) const;
        int findBlock(const char* name, int start_after = -1) const
        {
            return findBlock(getNameId(name), start_after);
        }

        /// Finds block by name id (in reverse order)
        int findBlockRev(int name_id, int start_before) const;

        /// Returns pointer to sub-block with specified name, or NULL if not found.
        const DataBlock* getBlockByName(const char* nm, int _after) const
        {
            return getBlockByName(getNameId(nm), _after, false);
        }
        DataBlock* getBlockByName(const char* name, int _after)
        {
            return getBlockByName(getNameId(name), _after, false);
        }

        /// functions that take only name now check that only one such block exists
        const DataBlock* getBlockByName(const char* name) const
        {
            return getBlockByName(getNameId(name), -1, true);
        }
        DataBlock* getBlockByName(const char* name)
        {
            return getBlockByName(getNameId(name), -1, true);
        }

        bool blockExists(const char* name) const
        {
            return getBlockByName(getNameId(name), -1, false) != NULL;
        }

        /// Get sub-block by name, returns @b def_blk, if not found.
        const DataBlock* getBlockByNameEx(const char* name, const DataBlock* def_blk) const
        {
            const DataBlock* blk = getBlockByName(getNameId(name), -1, true);
            return blk ? blk : def_blk;
        }

        /// Get block by name, returns (always valid) @b emptyBlock, if not found.
        const DataBlock* getBlockByNameEx(const char* name) const
        {
            return getBlockByNameEx(name, &emptyBlock);
        }

        /// Add block or get existing one.
        /// See also addNewBlock() and getBlockByNameEx(const char *name) const.
        DataBlock* addBlock(const char* name);

        /// Add new block. See also addBlock().
        DataBlock* addNewBlock(const char* name);

        /// Copies all parameters (not sub-blocks!) from specified DataBlock.
        void setParamsFrom(const DataBlock* copy_from)
        {
            clearParams();
            copy_from->addParamsTo(*this);
        }

        /// Copies all parameters (not sub-blocks!) from specified DataBlock.
        void setParamsFrom(const RoDataBlock& copy_from);

        /// Copies all parameters (not sub-blocks!) from specified DataBlock without erasing old ones.
        void appendParamsFrom(const DataBlock* copy_from);

        /// Create new block as a copy of specified DataBlock, with copy of all its sub-tree.
        /// @note Specified DataBlock can be from different DataBlock tree.
        DataBlock* addNewBlock(const DataBlock* copy_from, const char* as_name = NULL);

        /// Remove all sub-blocks with specified name.
        /// Returns false if no sub-blocks were removed.
        bool removeBlock(const char* name);

        /// Remove sub-block with specified index.
        /// Returns false on error.
        bool removeBlock(uint32_t i);

        /// Similar to addNewBlock(const DataBlock *copy_from, const char *as_name),
        /// but removes existing sub-blocks with the same name first.
        DataBlock* setBlock(const DataBlock* blk, const char* as_name = NULL)
        {
            if(!blk)
                return NULL;
            removeBlock(as_name ? as_name : blk->getBlockName());
            return addNewBlock(blk, as_name);
        }

        /// Clears data, then copies all parameters and sub-blocks from specified DataBlock.
        void setFrom(const DataBlock* from, const char* fname = nullptr);

        /// Clears data, then copies all parameters and sub-blocks from specified DataBlock.
        void setFrom(const RoDataBlock& from);
        /// @}

        /// @name Parameters - Getting and Enumeration
        /// @{

        /// Returns number of parameters in this DataBlock.
        /// Use for enumeration.
        uint32_t paramCount() const
        {
            return paramsCount;
        }

        int paramCountById(int name_id) const;
        int paramCountByName(const char* name) const
        {
            return paramCountById(getNameId(name));
        }

        /// Returns type of i-th parameter. See ParamType enum.
        int getParamType(uint32_t i) const;

        /// Returns i-th parameter name id. See getNameId().
        int getParamNameId(uint32_t i) const;

        /// Returns i-th parameter name. Uses getName().
        const char* getParamName(uint32_t i) const
        {
            return getName(getParamNameId(i));
        }

        /// Changes name of i-th parameter
        void changeParamName(uint32_t i, const char* name);

        /// Find parameter by name id.
        /// Returns parameter index or -1 if not found.
        int findParam(int name_id) const;
        int findParam(int name_id, int start_after) const;

        /// Finds parameter by name id (in reverse order), returns parameter index or -1 if not found.
        int findParamRev(int name_id, int start_before) const;

        /// Find parameter by name. Uses getNameId().
        /// Returns parameter index or -1 if not found.
        int findParam(const char* name, int _after = -1) const
        {
            return findParam(getNameId(name), _after);
        }

        /// Returns true if there is parameter with specified name id in this DataBlock.
        bool paramExists(int name_id, int _after = -1) const
        {
            return findParam(name_id, _after) >= 0;
        }

        /// Returns true if there is parameter with specified name in this DataBlock.
        bool paramExists(const char* name, int _after = -1) const
        {
            return findParam(name, _after) >= 0;
        }

        /// @}

        template <class T>
        T getByName(const char* name) const;
        template <class T>
        T getByName(const char* name, const T& def) const;
        template <class T>
        T getByNameId(int paramNameId, const T& def) const;

#define TYPE_FUNCTION_3(CppType, CRefType, ApiName)                      \
    CppType get##ApiName(int param_idx) const;                           \
    CppType get##ApiName(const char* name, CRefType def) const;          \
    CppType get##ApiName(const char* name) const;                        \
    CppType get##ApiName##ByNameId(int paramNameId, CRefType def) const; \
    bool set##ApiName(int param_idx, CRefType value);                    \
    int set##ApiName##ByNameId(int name_id, CRefType value);             \
    int set##ApiName(const char* name, CRefType value);                  \
    int add##ApiName(const char* name, CRefType value);                  \
    int addNew##ApiName##ByNameId(int name_id, CRefType value);

#define TYPE_FUNCTION(CppType, ApiName) TYPE_FUNCTION_3(CppType, CppType, ApiName)
#define TYPE_FUNCTION_CR(CppType, ApiName) TYPE_FUNCTION_3(CppType, const CppType&, ApiName)

        typedef const char* string_t;
        TYPE_FUNCTION(string_t, Str)
        TYPE_FUNCTION(int, Int)
        TYPE_FUNCTION(nau::math::E3DCOLOR, E3dcolor)
        TYPE_FUNCTION(int64_t, Int64)
        TYPE_FUNCTION(float, Real)
        TYPE_FUNCTION(bool, Bool)
        TYPE_FUNCTION_CR(Vectormath::Vector2, Point2)
        TYPE_FUNCTION_CR(Vectormath::Vector3, Point3)
        TYPE_FUNCTION_CR(Vectormath::Vector4, Point4)
        TYPE_FUNCTION_CR(Vectormath::IVector2, IPoint2)
        TYPE_FUNCTION_CR(Vectormath::IVector3, IPoint3)
        TYPE_FUNCTION_CR(Vectormath::Matrix4, Tm)
#undef TYPE_FUNCTION
#undef TYPE_FUNCTION_CR
#undef TYPE_FUNCTION_3

        /// @name Parameters - Removing
        /// @{

        /// Remove all parameters with the specified name.
        /// Returns false if no parameters were removed.
        bool removeParam(const char* name);

        /// Remove param by index
        bool removeParam(uint32_t i);

        /// @}

        void appendNamemapToSharedNamemap(DBNameMap& shared_nm, const DBNameMap* skip = nullptr) const;
        bool saveBinDumpWithSharedNamemap(iosys::IGenSave& cwr, const DBNameMap* shared_nm, bool pack = false, const ZSTD_CDict_s* dict = nullptr) const;
        bool loadBinDumpWithSharedNamemap(iosys::IGenLoad& crd, const DBNameMap* shared_nm, const ZSTD_DDict_s* dict = nullptr);

        /// Save DataBlock tree to binary stream (optionally using namemap stored separately)
        bool saveDumpToBinStream(iosys::IGenSave& cwr, const DBNameMap* ro) const;
        /// Load DataBlock tree from binary stream using namemap stored separately
        bool loadFromBinDump(iosys::IGenLoad& cr, const DBNameMap* ro);

    protected:
        static constexpr int INPLACE_PARAM_SIZE = 4;
        struct Param
        {
            uint32_t nameId : 24;
            uint32_t type : 8;
            uint32_t v;
        };
        typedef DataBlock* block_id_t;

        DataBlockShared* shared = nullptr;
        static constexpr uint32_t NAME_ID_MASK = (1 << 30) - 1;
        static constexpr uint32_t IS_TOPMOST = (1 << 31);  // owner
        uint32_t nameIdAndFlags = 0;
        uint16_t paramsCount = 0, blocksCount = 0;
        uint32_t firstBlockId = 0;
        uint32_t ofs = 0;  // RO param data starts here.
        DataBlockOwned* data = nullptr;

        friend struct DbUtils;
        friend class DataBlockParser;
        template <typename Cb>
        friend void dblk::iterate_child_blocks(const DataBlock& db, Cb cb);
        template <typename Cb>
        friend void dblk::iterate_blocks(const DataBlock& db, Cb cb);
        template <typename Cb>
        friend void dblk::iterate_blocks_lev(const DataBlock& db, Cb cb, int lev);
        template <typename Cb>
        friend void dblk::iterate_params(const DataBlock& db, Cb cb);

        uint32_t getNameIdIncreased() const
        {
            return nameIdAndFlags & NAME_ID_MASK;
        }

    protected:
        DataBlock(DataBlockShared* s, const char* nm);
        DataBlock(DataBlockShared* s, int nid, uint16_t pcnt, uint16_t bcnt, uint32_t fBlock, uint32_t ofs_);

        void saveToBinStreamWithoutNames(const DataBlockShared& names, iosys::IGenSave& cwr) const;
        template <bool print_with_limits>
        bool writeText(iosys::IGenSave& cwr, int lev, int* max_ln, int max_lev) const;

        bool loadText(const char* text, int text_length, const char* fname, DataBlock::IFileNotify* fnotify);
        bool loadFromStream(iosys::IGenLoad& crd, const char* fname, DataBlock::IFileNotify* fnotify, unsigned hint_size);
        bool load(const char* fname, DataBlock::IFileNotify* fnotify);
        bool doLoadFromStreamBBF3(iosys::IGenLoad& cwr);
        bool loadFromStreamBBF3(iosys::IGenLoad& cwr, const int* cnv, const int* cnvEnd, const DBNameMap& strings);

        int addParam(const char* name, int type, const char* value, const char* val_end, int line, const char* filename, int at = -1);
        void addParamsTo(DataBlock& dest) const;
        void clearParams();

        size_t memUsed_() const;

        const DataBlock* const* getBlockRWPtr() const;
        const DataBlock* getBlockROPtr() const;

        DataBlock* getBlockRW(uint32_t i);
        DataBlock* getBlockRO(uint32_t i);
        int findBlockRO(int nid, int after) const;
        int findBlockRW(int nid, int after) const;

        template <bool rw>
        int findParam(int name_id) const;
        template <bool rw>
        int findParam(int name_id, int start_after) const;

        template <class T, bool check_name_id>
        int setByNameId(int paramNameId, const T& val);
        template <class T>
        int setByName(const char* name, const T& val)
        {
            return setByNameId<T, false>(addNameId(name), val);
        }
        template <class T>
        int addByName(const char* name, const T& val)
        {
            return addByNameId<T, false>(addNameId(name), val);
        }

        template <class T, bool check_name_id>
        int addByNameId(int name_id, const T& val);
        template <class T>
        int insertParamAt(uint32_t at, uint32_t name_id, const T& v);

        template <class T, bool rw>
        T getByName(const char* name) const
        {
            const int nid = getNameId(name);
            const int pid = findParam<rw>(nid);
            if(pid >= 0)
                return get<T, rw>(pid);
            issue_error_missing_param(name, TypeOf<T>::type);
            return T();
        }
        template <class T, bool rw>
        T getByNameId(int paramNameId, const T& def) const;

        template <class T>
        T get(uint32_t param_idx, const T& def) const
        {
            return isOwned() ? get<T, true>(param_idx, def) : get<T, false>(param_idx, def);
        }
        template <class T>
        T get(uint32_t param_idx) const
        {
            return isOwned() ? get<T, true>(param_idx) : get<T, false>(param_idx);
        }

        template <class T, bool rw>
        T get(uint32_t param_idx) const;
        template <class T, bool rw>
        T get(uint32_t param_idx, const T& def) const;
        template <class T>
        bool set(uint32_t param_idx, const T& v);

        uint32_t allocateNewString(string_t v, size_t vlen);  // returns raw data ofs in rw
        uint32_t insertNewString(string_t v, size_t vlen);    // returns string id
        template <class T, bool rw>
        T getParamString(int) const;

        template <bool rw>
        const Param* getCParams() const
        {
            return (const Param*)(rw ? rwDataAt(0) : roDataAt(ofs));
        }
        template <bool rw>
        Param* getParams()
        {
            return (Param*)(rw ? rwDataAt(0) : roDataAt(ofs));
        }
        template <bool rw>
        const Param* getParams() const
        {
            return getCParams<rw>();
        }
        template <bool rw>
        Param& getParam(uint32_t i);
        template <bool rw>
        const Param& getParam(uint32_t i) const;

        __forceinline Param* getParamsImpl();
        __forceinline const Param* getParamsImpl() const;
        const Param* getParamsPtr() const;

        static uint32_t& getParamV(Param& p)
        {
            return p.v;
        }
        static const uint32_t& getParamV(const Param& p)
        {
            return p.v;
        }
        const char* getParamData(const Param& p) const;
        __forceinline void insertNewParamRaw(uint32_t pid, uint32_t nameId, uint32_t type, size_t type_size, const char* data);

        const char* roDataAt(uint32_t at) const;
        const char* rwDataAt(uint32_t at) const;
        char* rwDataAt(uint32_t at);

        template <class T>
        inline T return_string(const char*) const
        {
            return T();
        };

        template <class T>
        T& getRW(uint32_t at)
        {
            return *(T*)rwDataAt(at);
        }

        DataBlockOwned& getData();
        const DataBlockOwned& getData() const;

        static constexpr uint32_t IS_OWNED = ~uint32_t(0);
        int firstBlock() const
        {
            return firstBlockId;
        }
        // probably can be optimized, by assuming ofs is never 0 and firstBlockId is also never 0
        bool isOwned() const
        {
            return ofs == IS_OWNED;
        }
        bool isBlocksOwned() const
        {
            return firstBlockId == IS_OWNED;
        }
        __forceinline void toOwned()
        {
            if(!isOwned())
                convertToOwned();
        }

        uint32_t blocksOffset() const;
        uint32_t getUsedSize() const;
        void convertToOwned();
        void convertToBlocksOwned();
        uint32_t complexParamsSize() const;
        char* insertAt(uint32_t at, uint32_t n);
        char* insertAt(uint32_t at, uint32_t n, const char* v);
        void createData();
        void deleteShared();

        void issue_error_missing_param(const char* pname, int type) const;
        void issue_error_missing_file(const char* fname, const char* desc) const;
        void issue_error_load_failed(const char* fname, const char* desc) const;
        void issue_error_load_failed_ver(const char* fname, unsigned req_ver, unsigned file_ver) const;
        void issue_error_parsing(const char* fname, int curLine, const char* msg, const char* curLineP) const;
        void issue_error_bad_type(const char* pname, int type_new, int type_prev, const char* fname) const;
        void issue_error_bad_type(int pnid, int type_new, int type_prev) const;
        void issue_error_bad_value(const char* pname, const char* value, int type, const char* fname, int line) const;
        void issue_error_bad_type_get(int bnid, int pnid, int type_get, int type_data) const;
        void issue_warning_huge_string(const char* pname, const char* value, const char* fname, int line) const;
        void issue_deprecated_type_change(int pnid, int type_new, int type_prev) const;
    };

#undef INLINE

    namespace dblk
    {
        /// Load BLK from specified file
        NAU_KERNEL_EXPORT bool load(DataBlock& blk, const char* fname, ReadFlags flg = ReadFlags(), DataBlock::IFileNotify* fnotify = nullptr);

        /// Load BLK from text (fname is used only for error reporting)
        NAU_KERNEL_EXPORT bool load_text(DataBlock& blk, const eastl::span<char> text, ReadFlags flg = ReadFlags(), const char* fname = nullptr, DataBlock::IFileNotify* fnotify = nullptr);

        /// Load BLK from stream (fname is used only for error reporting)
        NAU_KERNEL_EXPORT bool load_from_stream(DataBlock& blk, iosys::IGenLoad& crd, ReadFlags flg = ReadFlags(), const char* fname = nullptr, DataBlock::IFileNotify* fnotify = nullptr, unsigned hint_size = 128);

        /// Save BLK (and its sub-tree) to the specified file (text form)
        NAU_KERNEL_EXPORT bool save_to_text_file(const DataBlock& blk, const char* filename);

        /// Save BLK (and its sub-tree) to the specified file (text form) in compact form
        NAU_KERNEL_EXPORT bool save_to_text_file_compact(const DataBlock& blk, const char* filename);

        /// Save BLK (and its sub-tree) to the specified file (binary form)
        NAU_KERNEL_EXPORT bool save_to_binary_file(const DataBlock& blk, const char* filename);

        /// Print this DataBlock (and its sub-tree) to the arbitrary text stream with limitations; returns true when whole BLK is written
        NAU_KERNEL_EXPORT bool print_to_text_stream_limited(const DataBlock& blk, iosys::IGenSave& cwr, int max_out_line_num = -1, int max_level_depth = -1, int init_indent = 0);

        /// Save BLK (and its sub-tree) to the specified file (binary form, packed format)
        NAU_KERNEL_EXPORT bool pack_to_binary_file(const DataBlock& blk, const char* filename, int approx_sz = 16 << 10);

        /// Save BLK (and its sub-tree) to arbitrary stream (binary form, packed format)
        NAU_KERNEL_EXPORT void pack_to_stream(const DataBlock& blk, iosys::IGenSave& cwr, int approx_sz = 16 << 10);

        /// Export BLK as JSON to the arbitrary stream (text form)
        NAU_KERNEL_EXPORT bool export_to_json_text_stream(const DataBlock& blk, iosys::IGenSave& cwr, bool allow_unquoted = false, int max_param_per_ln = 4, int max_block_per_ln = 1);

        /// returns currently set flags of BLK object
        NAU_KERNEL_EXPORT ReadFlags get_flags(const DataBlock& blk);
        /// sets additional flags to BLK object
        NAU_KERNEL_EXPORT void set_flag(DataBlock& blk, ReadFlags flg_to_add);
        /// clear flags of BLK object
        NAU_KERNEL_EXPORT void clr_flag(DataBlock& blk, ReadFlags flg_to_clr);

        static inline const char* resolve_type(uint32_t type)
        {
            static const char* types[DataBlock::TYPE_COUNT + 1] = {
                "none", "string", "int", "real", "point2", "point3", "point4", "ipoint2", "ipoint3", "bool", "e3dcolor", "tm", "int64", "unknown"};
            type = type < DataBlock::TYPE_COUNT ? type : DataBlock::TYPE_COUNT;
            return types[type];
        }

        static inline const char* resolve_short_type(uint32_t type)
        {
            static const char* types[DataBlock::TYPE_COUNT + 1] = {
                "none", "t", "i", "r", "p2", "p3", "p4", "ip2", "ip3", "b", "c", "m", "i64", "err"};
            type = type < DataBlock::TYPE_COUNT ? type : DataBlock::TYPE_COUNT;
            return types[type];
        }

        static inline uint32_t get_type_size(uint32_t type)
        {
            static const uint8_t sizes[DataBlock::TYPE_COUNT + 1] = {0, 8, 4, 4, 8, 12, 16, 8, 12, 1, 4, 12 * 4, 8, 0};
            type = type < DataBlock::TYPE_COUNT ? type : DataBlock::TYPE_COUNT;
            return sizes[type];
        }

        static inline bool is_ident_char(char c)
        {
            return ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || (c >= 'A' && c <= 'Z') || c == '-' || c == '.' || c == '~');
        }

        extern NAU_KERNEL_EXPORT const char* SHARED_NAMEMAP_FNAME;

        NAU_KERNEL_EXPORT DBNameMap* create_db_names();
        NAU_KERNEL_EXPORT void destroy_db_names(DBNameMap* nm);
        NAU_KERNEL_EXPORT bool write_names(iosys::IGenSave& cwr, const DBNameMap& names, uint64_t* names_hash);
        NAU_KERNEL_EXPORT bool read_names(iosys::IGenLoad& cr, DBNameMap& names, uint64_t* names_hash);
        NAU_KERNEL_EXPORT int db_names_count(DBNameMap*);

        /* TODO: VROM integration
        //! returns shared name map for specified vromfs (acquires reference)
        NAU_KERNEL_EXPORT DBNameMap *get_vromfs_shared_name_map(const VirtualRomFsData *fs);
        //! releases reference to shared name map; returns residual ref count
        NAU_KERNEL_EXPORT int release_vromfs_shared_name_map(DBNameMap *nm);

        //! returns ZSTD decoder dictionary for specified vromfs (acquires reference)
        NAU_KERNEL_EXPORT ZSTD_DDict_s *get_vromfs_blk_ddict(const VirtualRomFsData *fs);
        //! releases reference to ZSTD decoder dictionary; returns residual ref count
        NAU_KERNEL_EXPORT int release_vromfs_blk_ddict(ZSTD_DDict_s *ddict);

        //! discard unused shared name maps (that were allocated in get_vromfs_shared_name_map() calls); return residual used count
        NAU_KERNEL_EXPORT int discard_unused_shared_name_maps();
        //! discard unused ZSTD decoder dictionaries (that were allocated in get_vromfs_blk_ddict() calls); return residual used count
        NAU_KERNEL_EXPORT int discard_unused_blk_ddict();

        //! returns ZSTD encoder dictionary for specified vromfs created with zstd_create_cdict (to be released with zstd_destroy_cdict)
        NAU_KERNEL_EXPORT ZSTD_CDict_s *create_vromfs_blk_cdict(const VirtualRomFsData *fs, int compr_level);
        */

        template <typename Cb>
        static inline void iterate_child_blocks(const DataBlock& db, Cb cb);
        template <typename Cb>
        static inline void iterate_blocks(const DataBlock& db, Cb cb);
        template <typename Cb>
        static inline void iterate_child_blocks_by_name_id(const DataBlock& db, int name_id, Cb cb);
        template <typename Cb>
        static inline void iterate_child_blocks_by_name(const DataBlock& db, const char* nm, Cb cb);
        template <typename Cb>
        static inline void iterate_blocks_by_name_id(const DataBlock& db, int name_id, Cb cb);
        template <typename Cb>
        static inline void iterate_blocks_by_name(const DataBlock& db, const char* nm, Cb cb);

        template <typename Cb>
        static inline void iterate_params(const DataBlock& db, Cb cb);
        template <typename Cb>
        static inline void iterate_params_by_type(const DataBlock& db, int type, Cb cb);
        template <typename Cb>
        static inline void iterate_params_by_name_id(const DataBlock& db, int name_id, Cb cb);
        template <typename Cb>
        static inline void iterate_params_by_name(const DataBlock& db, const char* name, Cb cb);
        template <typename Cb>
        static inline void iterate_params_by_name_id_and_type(const DataBlock& db, int name_id, int type, Cb cb);
        template <typename Cb>
        static inline void iterate_params_by_name_and_type(const DataBlock& db, const char* nm, int type, Cb cb);

    }  // namespace dblk

    template <>
    struct DataBlock::TypeOf<int>
    {
        static constexpr int type = DataBlock::TYPE_INT;
    };
    template <>
    struct DataBlock::TypeOf<float>
    {
        static constexpr int type = DataBlock::TYPE_REAL;
    };
    template <>
    struct DataBlock::TypeOf<bool>
    {
        static constexpr int type = DataBlock::TYPE_BOOL;
    };
    template <>
    struct DataBlock::TypeOf<nau::math::E3DCOLOR>
    {
        static constexpr int type = DataBlock::TYPE_E3DCOLOR;
    };
    template <>
    struct DataBlock::TypeOf<int64_t>
    {
        static constexpr int type = DataBlock::TYPE_INT64;
    };
    template <>
    struct DataBlock::TypeOf<Vectormath::IVector2>
    {
        static constexpr int type = DataBlock::TYPE_IPOINT2;
    };
    template <>
    struct DataBlock::TypeOf<Vectormath::IVector3>
    {
        static constexpr int type = DataBlock::TYPE_IPOINT3;
    };
    template <>
    struct DataBlock::TypeOf<Vectormath::Vector2>
    {
        static constexpr int type = DataBlock::TYPE_POINT2;
    };
    template <>
    struct DataBlock::TypeOf<Vectormath::Vector3>
    {
        static constexpr int type = DataBlock::TYPE_POINT3;
    };
    template <>
    struct DataBlock::TypeOf<Vectormath::Vector4>
    {
        static constexpr int type = DataBlock::TYPE_POINT4;
    };
    template <>
    struct DataBlock::TypeOf<Vectormath::Matrix4>
    {
        static constexpr int type = DataBlock::TYPE_MATRIX;
    };
    template <>
    struct DataBlock::TypeOf<const char*>
    {
        static constexpr int type = DataBlock::TYPE_STRING;
    };

    template <>
    bool DataBlock::set<DataBlock::string_t>(uint32_t param_idx, const DataBlock::string_t& v);
    template <>
    inline DataBlock::string_t DataBlock::return_string<DataBlock::string_t>(const char* s) const
    {
        return s;
    }

    template <class T>
    inline T DataBlock::getByName(const char* name) const
    {
        return isOwned() ? getByName<T, true>(name) : getByName<T, false>(name);
    }

    template <class T>
    inline T DataBlock::getByName(const char* name, const T& def) const
    {
        const int id = getNameId(name);
        return id < 0 ? def : getByNameId(id, def);
    }

    template <class T>
    inline T DataBlock::getByNameId(int paramNameId, const T& def) const
    {
        return isOwned() ? getByNameId<T, true>(paramNameId, def) : getByNameId<T, false>(paramNameId, def);
    }

    inline bool DataBlock::saveToTextFile(const char* fn) const
    {
        return dblk::save_to_text_file(*this, fn);
    }

    inline bool DataBlock::saveToTextFileCompact(const char* fn) const
    {
        return dblk::save_to_text_file_compact(*this, fn);
    }

    inline bool DataBlock::printToTextStreamLimited(iosys::IGenSave& cwr, int max_out_line_num, int max_level_depth, int init_indent) const
    {
        return dblk::print_to_text_stream_limited(*this, cwr, max_out_line_num, max_level_depth, init_indent);
    }

    template <typename Cb>
    static inline void dblk::iterate_child_blocks(const DataBlock& db, Cb cb)
    {
        if(const DataBlock* b = db.getBlockROPtr())
        {
            for(const DataBlock* be = b + db.blockCount(); b < be; b++)
                cb(*b);
        }
        else if(const DataBlock* const* b = db.getBlockRWPtr())
        {
            for(const DataBlock* const* be = b + db.blockCount(); b < be; b++)
                cb(**b);
        }
    }
    template <typename Cb>
    static inline void dblk::iterate_blocks(const DataBlock& db, Cb cb)
    {
        cb(db);
        iterate_child_blocks(db, [&](const DataBlock& b)
        {
            dblk::iterate_blocks(b, cb);
        });
    }
    template <typename Cb>
    static inline void dblk::iterate_blocks_lev(const DataBlock& db, Cb cb, int lev)
    {
        cb(db, lev);
        if(const DataBlock* b = db.getBlockROPtr())
        {
            for(const DataBlock* be = b + db.blockCount(); b < be; b++)
                iterate_blocks_lev(*b, cb, lev + 1);
        }
        else if(const DataBlock* const* b = db.getBlockRWPtr())
        {
            for(const DataBlock* const* be = b + db.blockCount(); b < be; b++)
                iterate_blocks_lev(**b, cb, lev + 1);
        }
    }
    template <typename Cb>
    static inline void dblk::iterate_child_blocks_by_name_id(const DataBlock& db, int nid, Cb cb)
    {
        iterate_child_blocks(db, [&](const DataBlock& b)
        {
            if(b.getBlockNameId() == nid)
                cb(b);
        });
    }
    template <typename Cb>
    static inline void dblk::iterate_child_blocks_by_name(const DataBlock& db, const char* nm, Cb cb)
    {
        int nid = db.getNameId(nm);
        if(nid >= 0)
            iterate_child_blocks_by_name_id(db, nid, cb);
    }
    template <typename Cb>
    static inline void dblk::iterate_blocks_by_name_id(const DataBlock& db, int nid, Cb cb)
    {
        if(db.getBlockNameId() == nid)
            cb(db);
        iterate_child_blocks_by_name_id(db, nid, [&](const DataBlock& b)
        {
            iterate_blocks_by_name_id(b, nid, cb);
        });
    }
    template <typename Cb>
    static inline void dblk::iterate_blocks_by_name(const DataBlock& db, const char* nm, Cb cb)
    {
        int nid = db.getNameId(nm);
        if(nid >= 0)
            dblk::iterate_blocks_by_name_id(db, nid, cb);
    }

    template <typename Cb>
    static inline void dblk::iterate_params(const DataBlock& db, Cb cb)
    {
        const DataBlock::Param *s = db.getParamsPtr(), *e = s + db.paramsCount;
        for(uint32_t i = 0; s < e; s++, i++)
            cb(i, s->nameId, s->type);
    }

    template <typename Cb>
    static inline void dblk::iterate_params_by_type(const DataBlock& db, int type, Cb cb)
    {
        iterate_params(db, [&](int param_idx, int param_name_id, int param_type)
        {
            if(param_type == type)
                cb(param_idx, param_name_id, param_type);
        });
    }

    template <typename Cb>
    static inline void dblk::iterate_params_by_name_id(const DataBlock& db, int name_id, Cb cb)
    {
        iterate_params(db, [&](int param_idx, int param_name_id, int param_type)
        {
            if(param_name_id == name_id)
                cb(param_idx, param_name_id, param_type);
        });
    }
    template <typename Cb>
    static inline void dblk::iterate_params_by_name(const DataBlock& db, const char* nm, Cb cb)
    {
        int name_id = db.getNameId(nm);
        if(name_id >= 0)
            iterate_params_by_name_id(db, name_id, cb);
    }

    template <typename Cb>
    static inline void dblk::iterate_params_by_name_id_and_type(const DataBlock& db, int name_id, int type, Cb cb)
    {
        iterate_params(db, [&](int param_idx, int param_name_id, int param_type)
        {
            if(param_name_id == name_id && param_type == type)
                cb(param_idx);
        });
    }
    template <typename Cb>
    static inline void dblk::iterate_params_by_name_and_type(const DataBlock& db, const char* nm, int type, Cb cb)
    {
        int name_id = db.getNameId(nm);
        if(name_id >= 0)
            iterate_params_by_name_id_and_type(db, name_id, type, cb);
    }

    /// @}

    /// @}
}  // namespace nau

DAG_DECLARE_RELOCATABLE(nau::DataBlock);