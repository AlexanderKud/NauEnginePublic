// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#include "nau/asset_tools/compilers/input_compilers.h"

#include <EASTL/vector.h>
#include <nau/shared/logger.h>
#include <pxr/usd/sdf/fileFormat.h>

#include "nau/asset_tools/asset_utils.h"
#include "nau/asset_tools/db_manager.h"

#include "nau/assets/animation_asset_accessor.h"
#include "nau/assets/ui_asset_accessor.h"

#include "nau/dataBlock/dag_dataBlock.h"

#include "nau/io/stream_utils.h"

#include "nau/usd_meta_tools/usd_meta_info.h"

#include "pxr/usd/usd/prim.h"
#include "pxr/usd/usd/stage.h"
#include "usd_proxy/usd_prim_proxy.h"

namespace nau::compilers
{
    // TODO True VFX compiler
    nau::Result<AssetMetaInfo> UsdInputCompiler::compile(
        PXR_NS::UsdStageRefPtr stage,
        const std::string& outputPath,
        const std::string& projectRootPath,
        const nau::UsdMetaInfo& metaInfo,
        int folderIndex)
    {
        AssetDatabaseManager& dbManager = AssetDatabaseManager::instance();
        NAU_ASSERT(dbManager.isLoaded(), "Asset database not loaded!");

        auto rootPrim = stage->GetPrimAtPath(pxr::SdfPath("/Root"));
        if (!rootPrim) {
            return NauMakeError("Can't load source stage from '{}'", metaInfo.assetPath);
        }

        auto proxyPrim = UsdProxy::UsdProxyPrim(rootPrim);

        std::string stringUID;
        auto uidProperty = proxyPrim.getProperty(pxr::TfToken("uid"));
        if (uidProperty)
        {
            pxr::VtValue val;
            uidProperty->getValue(&val);

            if (val.IsHolding<std::string>())
            {
                stringUID = val.Get<std::string>();
            }
        }

        AssetMetaInfo ninputMeta;
        const std::string relativeSourcePath = FileSystemExtensions::getRelativeAssetPath(metaInfo.assetPath, false).string();
        const std::string sourcePath = std::format("{}", relativeSourcePath.c_str());

        auto id = dbManager.findIf(sourcePath);

        if (id.isError() && !stringUID.empty())
        {
            id = Uid::parseString(stringUID);
        }

        const std::filesystem::path out = std::filesystem::path(outputPath) / std::to_string(folderIndex);
        std::string fileName = toString(*id) + ".ninput";

        ninputMeta.uid = *id;
        ninputMeta.dbPath = (out.filename() / fileName).string().c_str();
        ninputMeta.sourcePath = sourcePath.c_str();
        ninputMeta.nausdPath = (sourcePath + ".nausd").c_str();
        ninputMeta.dirty = false;
        ninputMeta.kind = "Input";

        auto outFilePath = utils::compilers::ensureOutputPath(outputPath, ninputMeta, "");

        DataBlock outBlk;
        outBlk.setStr("dummy", "dummyInput");
        bool bRet = outBlk.saveToTextFile(outFilePath.string().c_str());

        if (bRet)
        {
            dbManager.addOrReplace(ninputMeta);
            return ninputMeta;
        }

        return NauMakeError("Input asset loading failed");
    }

}  // namespace nau::compilers
