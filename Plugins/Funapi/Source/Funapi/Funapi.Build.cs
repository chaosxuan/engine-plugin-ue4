// Copyright 1998-2016 Epic Games, Inc. All Rights Reserved.

using System.IO;

namespace UnrealBuildTool.Rules
{
	public class Funapi : ModuleRules
	{
		public Funapi(TargetInfo Target)
		{
            PublicIncludePaths.AddRange(
				new string[] {
					// ... add public include paths required here ...
                    "Funapi/Pubilc",
                    "Funapi/Pubilc/funapi",
                    "Funapi/Pubilc/funapi/management",
                    "Funapi/Pubilc/funapi/network",
                    "Funapi/Pubilc/funapi/service",
                }
			);

			PrivateIncludePaths.AddRange(
				new string[] {
                    "Funapi/Private",
					// ... add other private include paths required here ...
				}
			);

			PublicDependencyModuleNames.AddRange(
				new string[]
				{
					"Core",
                    "Engine",
					// ... add other public dependencies that you statically link with here ...
				}
			);

			PrivateDependencyModuleNames.AddRange(
				new string[]
				{
                    "Core",
                    "Engine",
                    // "Json"
                    // "Http"
					// ... add private dependencies that you statically link with here ...
				}
            );

			DynamicallyLoadedModuleNames.AddRange(
				new string[]
				{
					// ... add any modules that your module loads dynamically here ...
				}
            );

            // Third party library
            var ThirdPartyPath = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", "..", "ThirdParty/"));

            // include path
            PublicIncludePaths.Add(Path.Combine(ThirdPartyPath, "include"));
            // PublicIncludePaths.Add(Path.GetFullPath(Path.Combine(ModuleDirectory, "funapi")));

            // definitions
            Definitions.Add("GOOGLE_PROTOBUF_NO_RTTI=1");
            Definitions.Add("PROTOBUF_USE_EXCEPTIONS=0");

            // library
            var LibPath = ThirdPartyPath;

            if (Target.Platform == UnrealTargetPlatform.Mac)
            {
                PublicIncludePaths.Add(Path.Combine(ThirdPartyPath, "include", "Mac"));

                LibPath += "lib/Mac";
                PublicAdditionalLibraries.Add(Path.Combine(LibPath, "libcurl.a"));
                PublicAdditionalLibraries.Add(Path.Combine(LibPath, "libcrypto.a"));
                PublicAdditionalLibraries.Add(Path.Combine(LibPath, "libssl.a"));
                PublicAdditionalLibraries.Add(Path.Combine(LibPath, "libz.a"));
                // PublicAdditionalLibraries.Add(Path.Combine(LibPath, "libprotobuf.a"));
                PublicAdditionalLibraries.Add(Path.Combine(LibPath, "libsodium.a"));
            }
            else if (Target.Platform == UnrealTargetPlatform.Win32)
            {
                PublicIncludePaths.Add(Path.Combine(ThirdPartyPath, "include", "Windows", "x86"));

                LibPath += "lib/Windows/x86";
                PublicLibraryPaths.Add(LibPath);

                PublicAdditionalLibraries.Add(Path.Combine(LibPath, "libcurl_a.lib"));
                PublicAdditionalLibraries.Add(Path.Combine(LibPath, "libeay32.lib"));
                // PublicAdditionalLibraries.Add(Path.Combine(LibPath, "libprotobuf.lib"));
                PublicAdditionalLibraries.Add(Path.Combine(LibPath, "libsodium.lib"));
                Definitions.Add("CURL_STATICLIB=1");
            }
            else if (Target.Platform == UnrealTargetPlatform.Win64)
            {
                PublicIncludePaths.Add(Path.Combine(ThirdPartyPath, "include", "Windows", "x64"));

                LibPath += "lib/Windows/x64";
                PublicLibraryPaths.Add(LibPath);

                PublicAdditionalLibraries.Add(Path.Combine(LibPath, "libcurl_a.lib"));
                // PublicAdditionalLibraries.Add(Path.Combine(LibPath, "libprotobuf.lib"));
                PublicAdditionalLibraries.Add(Path.Combine(LibPath, "libsodium.lib"));
                Definitions.Add("CURL_STATICLIB=1");
            }
            else if (Target.Platform == UnrealTargetPlatform.Android)
            {
                // toolchain will filter properly
                PublicIncludePaths.Add(LibPath + "include/Android/ARMv7");
                PublicLibraryPaths.Add(LibPath + "lib/Android/ARMv7");
                //PublicIncludePaths.Add(LibPath + "include/Android/ARM64");
                //PublicLibraryPaths.Add(LibPath + "lib/Android/ARM64");
                //PublicIncludePaths.Add(LibPath + "include/Android/x86");
                //PublicLibraryPaths.Add(LibPath + "lib/Android/x86");
                //PublicIncludePaths.Add(LibPath + "include/Android/x64");
                //PublicLibraryPaths.Add(LibPath + "lib/Android/x64");

                LibPath += "lib/Android/ARMv7";
                //PublicAdditionalLibraries.Add("curl");
                //PublicAdditionalLibraries.Add("crypto");
                //PublicAdditionalLibraries.Add("protobuf");

                PublicAdditionalLibraries.Add(Path.Combine(LibPath, "libcurl.a"));
                PublicAdditionalLibraries.Add(Path.Combine(LibPath, "libcrypto.a"));
                PublicAdditionalLibraries.Add(Path.Combine(LibPath, "libssl.a"));
                // PublicAdditionalLibraries.Add(Path.Combine(LibPath, "libprotobuf.a"));
                PublicAdditionalLibraries.Add(Path.Combine(LibPath, "libsodium.a"));
            }
            else if (Target.Platform == UnrealTargetPlatform.IOS)
            {
                PublicIncludePaths.Add(Path.Combine(ThirdPartyPath, "include", "iOS"));

                LibPath += "lib/iOS";
                PublicAdditionalLibraries.Add(Path.Combine(LibPath, "libcurl.a"));
                PublicAdditionalLibraries.Add(Path.Combine(LibPath, "libcrypto.a"));
                PublicAdditionalLibraries.Add(Path.Combine(LibPath, "libssl.a"));
                // PublicAdditionalLibraries.Add(Path.Combine(LibPath, "libprotobuf.a"));
                PublicAdditionalLibraries.Add(Path.Combine(LibPath, "libsodium.a"));
            }
        }
    }
}