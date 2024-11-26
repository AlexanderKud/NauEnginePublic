// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#include <filesystem>
#include <format>
#include <iostream>

#include "nau/diag/error.h"
#include "nau/io/file_system.h"
#include "nau/string/string_conv.h"
#include "nau/utils/result.h"
#include "shader_cache_builder.h"

namespace fs = std::filesystem;

using Args = nau::ShaderCacheBuilder::Arguments;

constexpr auto OutKey = "-o";
constexpr auto OutFullKey = "--out";

constexpr auto ShadersKey = "-s";
constexpr auto ShadersFullKey = "--shaders";

constexpr auto MetafilesKey = "-m";
constexpr auto MetafilesFullKey = "--metafiles";

constexpr auto IncludesKey = "-i";
constexpr auto IncludesFullKey = "--includes";

constexpr auto ShaderCacheKey = "-c";
constexpr auto ShaderCacheFullKey = "-cache";

constexpr auto DebugOutKey = "-Do";
constexpr auto DebugOutFullKey = "--debug-out";

constexpr auto DebugEmbedKey = "-De";
constexpr auto DebugEmbedFullKey = "--debug-embed";

constexpr auto Extension = ".nsbc";

nau::Result<Args> parseArguments(int argc, char* argv[]);

void printUsage(std::string_view appName);

int main(int argc, char* argv[])
{
    using namespace nau;

    auto args = parseArguments(argc, argv);
    if (args.isError())
    {
        std::cerr << strings::toStringView(args.getError()->getMessage()) << '\n';
        printUsage(argv[0]);
        return EXIT_FAILURE;
    }

    const bool isSingleSource = fs::is_regular_file(args->shadersPath) && fs::is_regular_file(args->metafilesPath);
    if (isSingleSource && args->shaderCacheName.empty())
    {
        args->shaderCacheName = std::format("{}{}", args->shadersPath.stem().string(), Extension);
    }

    nau::ShaderCacheBuilder builder;

    if (args->shaderCacheName.empty())
    {
        ShaderCacheBuilder::StreamFactory streamFactory = [&args](std::string_view shaderName) -> io::IStreamWriter::Ptr
        {
            fs::path fullPath = args->outDir / fs::path(shaderName);
            fullPath += Extension;

            const std::string fullPathStr = fullPath.string();

            return io::createNativeFileStream(fullPathStr.c_str(), io::AccessMode::Write, io::OpenFileMode::CreateAlways);
        };

        auto result = builder.makeCacheFiles(std::move(streamFactory), *args);
        if (result.isError())
        {
            std::cerr << strings::toStringView(result.getError()->getMessage()) << '\n';
            return EXIT_FAILURE;
        }

        std::cout << std::format("Done building shader cache files: {}\n", args->outDir.string());
    }
    else
    {
        ShaderCacheBuilder::StreamFactory streamFactory = [&args](std::string_view shaderName) -> io::IStreamWriter::Ptr
        {
            fs::path shaderCacheFilePath = args->outDir / args->shaderCacheName;
            const std::string ext = shaderCacheFilePath.extension().string();
            if (ext != Extension)
            {
                std::cout << std::format("Warning: invalid extension '{}' will be replaced to '{}'\n", ext, Extension);
                shaderCacheFilePath.replace_extension(Extension);
            }

            const std::string fullPathStr = shaderCacheFilePath.string();

            return io::createNativeFileStream(fullPathStr.c_str(), io::AccessMode::Write, io::OpenFileMode::CreateAlways);
        };

        auto result = builder.makeCache(std::move(streamFactory), *args);
        if (result.isError())
        {
            std::cerr << strings::toStringView(result.getError()->getMessage()) << '\n';
            return EXIT_FAILURE;
        }

        std::cout << std::format("Done building shader cache: {}\n", (args->outDir / args->shaderCacheName).string());
    }

    if (args->embedDebugInfo)
    {
        std::cout << "Debug info was embedded in bytecode.\n";
    }

    if (args->debugOutputDir.has_value())
    {
        std::cout << std::format("PDB files were saved to: {}\n", args->debugOutputDir->string());
    }

    return EXIT_SUCCESS;
}

nau::Result<Args> parseArguments(int argc, char* argv[])
{
    Args args;

    auto hasOutDir = false;
    auto hasShadersDir = false;
    auto hasMetafilesDir = false;

    for (auto i = 1; i < argc; ++i)
    {
        std::string_view arg = argv[i];

        if (arg == "-h" || arg == "--help")
        {
            printUsage(argv[0]);
            std::exit(EXIT_SUCCESS);
        }

        if (arg == OutKey || arg == OutFullKey)
        {
            if (i + 1 < argc && argv[i + 1][0] != '-')
            {
                args.outDir = argv[i + 1];
                if (!fs::create_directory(args.outDir) && !fs::is_directory(args.outDir))
                {
                    return NauMakeError("This is not a directory or does not exist ({}/{}): {}\n", OutKey, OutFullKey, args.outDir.string());
                }
                hasOutDir = true;
                i++;
            }
            else
            {
                return NauMakeError("Missing value for {}/{}", OutKey, OutFullKey);
            }
        }
        else if (arg == ShadersKey || arg == ShadersFullKey)
        {
            if (i + 1 < argc && argv[i + 1][0] != '-')
            {
                args.shadersPath = argv[i + 1];
                hasShadersDir = true;
                i++;
            }
            else
            {
                return NauMakeError("Missing value for {}/{}", ShadersKey, ShadersFullKey);
            }
        }
        else if (arg == MetafilesKey || arg == MetafilesFullKey)
        {
            if (i + 1 < argc && argv[i + 1][0] != '-')
            {
                args.metafilesPath = argv[i + 1];
                hasMetafilesDir = true;
                i++;
            }
            else
            {
                return NauMakeError("Missing value for {}/{}", MetafilesKey, MetafilesFullKey);
            }
        }
        else if (arg == ShaderCacheKey || arg == ShaderCacheFullKey)
        {
            if (i + 1 < argc && argv[i + 1][0] != '-')
            {
                args.shaderCacheName = argv[++i];
            }
            else
            {
                return NauMakeError("Missing value for {}/{}", ShaderCacheKey, ShaderCacheFullKey);
            }
        }
        else if (arg == IncludesKey || arg == IncludesFullKey)
        {
            while (i + 1 < argc && argv[i + 1][0] != '-')
            {
                args.includeDirs.emplace_back(argv[i + 1]);
                if (!fs::is_directory(args.includeDirs.back()))
                {
                    return NauMakeError("This is not a directory or does not exist ({}/{}): {}\n", IncludesKey, IncludesFullKey, args.includeDirs.back().string());
                }
                i++;
            }
        }
        else if (arg == DebugEmbedKey || arg == DebugEmbedFullKey)
        {
            args.embedDebugInfo = true;
        }
        else if (arg == DebugOutKey || arg == DebugOutFullKey)
        {
            if (i + 1 < argc && argv[i + 1][0] != '-')
            {
                args.debugOutputDir = argv[i + 1];
                if (!fs::create_directory(args.debugOutputDir.value()) && !fs::is_directory(args.debugOutputDir.value()))
                {
                    return NauMakeError("This is not a directory or does not exist ({}/{}): {}\n", DebugOutKey, DebugOutFullKey, args.debugOutputDir->string());
                }
                i++;
            }
            else
            {
                return NauMakeError("Missing value for {}/{}", DebugOutKey, DebugOutFullKey);
            }
        }
        else
        {
            return NauMakeError("Unknown argument: {}", arg);
        }
    }

    NAU_ASSERT((fs::is_directory(args.metafilesPath) && fs::is_directory(args.shadersPath)) || (fs::is_regular_file(args.metafilesPath) && fs::is_regular_file(args.shadersPath)), "Shaders and metafiles must either both be directories or both be files");

    if (!hasOutDir)
    {
        return NauMakeError("Missing required argument: {}/{}", OutKey, OutFullKey);
    }
    if (!hasShadersDir)
    {
        return NauMakeError("Missing required argument: {}/{}", ShadersKey, ShadersFullKey);
    }
    if (!hasMetafilesDir)
    {
        return NauMakeError("Missing required argument: {}/{}", MetafilesKey, MetafilesFullKey);
    }

    return args;
}

void printUsage(std::string_view appName)
{
    const fs::path fullName(appName);

    std::cout << std::format(
        "Usage: {} -o <output_directory> -s <shaders_path> -m <metafiles_path> "
        "[-i <include_path1> <include_path2> ...] "
        "[-c <shader_cache_name>] "
        "[-Do <pdb_output_directory>] "
        "[-De]\n",
        fullName.filename().string());

    std::cout << "\nOptions:\n";
    std::cout << "  -h, --help             Display this help message and exit.\n";
    std::cout << "  -o, --out              Output directory for shader cache files (required).\n";
    std::cout << "  -s, --shaders          Directory containing shader files or path to single source (required).\n";
    std::cout << "  -m, --metafiles        Directory containing metafiles or path to single metafile (required).\n";
    std::cout << "  -i, --includes         Additional include directories (optional).\n";
    std::cout << "  -c, --cache            Name of the shader cache file to be created (optional).\n";
    std::cout << "  -Do, --debug-out       Specify directory to output PDB files for debugging (optional).\n";
    std::cout << "  -De, --debug-embed     Embed debug information into the shader bytecode (optional).\n";
}
