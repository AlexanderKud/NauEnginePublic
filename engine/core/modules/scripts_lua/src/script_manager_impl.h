// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#pragma once

#include "nau/rtti/rtti_impl.h"
#include "nau/runtime/disposable.h"
#include "nau/scripts/script_manager.h"
#include "nau/service/service.h"


namespace nau::scripts
{
    class ScriptManagerImpl final : public ScriptManager,
                                    public IServiceInitialization,
                                    public IDisposable
    {
        NAU_RTTI_CLASS(nau::scripts::ScriptManagerImpl, ScriptManager, IServiceInitialization, IDisposable)

    public:
        ~ScriptManagerImpl();

    private:
        static int luaRequire(lua_State* l) noexcept;

        Result<Ptr<>> executeScriptFromBytes(const char* scriptName, eastl::span<const std::byte> scriptCode) override;

        Result<Ptr<>> executeScriptFromFile(const io::FsPath& path) override;

        void registerClass(IClassDescriptor::Ptr classDescriptor) override;

        Result<Ptr<IDispatch>> createScriptInstance(eastl::string_view scriptClass) override;

        void addScriptSearchPath(io::FsPath path) override;

        void addScriptFileExtension(eastl::string_view ext) override;

        async::Task<> preInitService() override;

        void dispose() override;

        Result<> invokeGlobal(eastl::string_view method, DispatchArguments args, Functor<void (const nau::Ptr<>& result)>) override;

        lua_State* getLua() const;

        Result<> executeFileInternal(const io::FsPath& filePath);


        lua_State* m_luaState = nullptr;
        eastl::vector<io::FsPath> m_searchPaths;
        eastl::string m_scriptFileExtension = ".lua";
    };

}  // namespace nau::scripts
