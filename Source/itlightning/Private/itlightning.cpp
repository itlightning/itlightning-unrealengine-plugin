// Copyright (C) 2024 IT Lightning, LLC. All rights reserved.
// Licensed under the MIT license - see LICENSE.md

#include "itlightning.h"
#include <GenericPlatformOutputDevices.h>
#include <OutputDeviceFile.h>

#define LOCTEXT_NAMESPACE "FitlightningModule"

DEFINE_LOG_CATEGORY(LogPluginITLightning);

class FITLLogOutputDeviceInitializer
{
public:
	TUniquePtr<FOutputDevice> LogDevice;
	FString LogFilePath;
	void InitLogDevice(const TCHAR* Filename)
	{
		if (!LogDevice)
		{
			FString ParentDir = FPaths::GetPath(FPaths::ConvertRelativePathToFull(FGenericPlatformOutputDevices::GetAbsoluteLogFilename()));
			LogFilePath = FPaths::Combine(ParentDir, Filename);
			LogDevice = MakeUnique<FOutputDeviceFile>(GetData(LogFilePath), /*bDisableBackup*/true, /*bAppendIfExists*/true, /*bCreateWriterLazily*/true, [](const TCHAR* AbsPathname) {});
		}
	}
};

FITLLogOutputDeviceInitializer& GetITLInternalGameLog() {
	// TODO: change the filename based on whether we're in the editor, client game, server, etc.
	static FITLLogOutputDeviceInitializer Singleton;
	Singleton.InitLogDevice(TEXT("itlightning-game.log"));
	return Singleton;
}

FITLLogOutputDeviceInitializer& GetITLInternalOpsLog() {
	static FITLLogOutputDeviceInitializer Singleton;
	Singleton.InitLogDevice(TEXT("itlightning-operations.log"));
	return Singleton;
}

void FitlightningModule::StartupModule()
{
	// Log all messages to the internal game log, which we will then read from the file as we push log data to the cloud
	GLog->AddOutputDevice(GetITLInternalGameLog().LogDevice.Get());
	UE_LOG(LogEngine, Log, TEXT("IT Lightning logger starting up..."));
	//TODO: is this the right way to read plugin config values? see OodleNetwork
	//FString ConfigFilePath = FPaths::ProjectConfigDir() + TEXT("Defaultitlightning.ini");
	//GConfig->LoadFile(ConfigFilePath);
	FString AgentID = GConfig->GetStr(ITL_CONFIG_SECTION_NAME, TEXT("AgentID"), GEngineIni);
	FString AuthToken = GConfig->GetStr(ITL_CONFIG_SECTION_NAME, TEXT("AuthToken"), GEngineIni);
	UE_LOG(LogEngine, Log, TEXT("IT Lightning config values: agentID=%s, authToken=%s"), GetData(AgentID), GetData(AuthToken));
}

void FitlightningModule::ShutdownModule()
{
	UE_LOG(LogEngine, Log, TEXT("IT Lightning logger shutting down..."));
	// TODO: try to flush pending logs to the cloud. If successful, then we should purge the itl game log and delete the progress marker to 0.
	// (fully flushed logs should not leave behind the itlightning-game.log file). That way, on a crash, we'll know we need to continue where we left off.
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FitlightningModule, itlightning)