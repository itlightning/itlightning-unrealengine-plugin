// Copyright (C) 2024 IT Lightning, LLC. All rights reserved.
// Licensed under the MIT license - see LICENSE

#include "itlightning.h"
#include <GenericPlatformOutputDevices.h>
#include <OutputDeviceFile.h>

#define LOCTEXT_NAMESPACE "FitlightningModule"

DEFINE_LOG_CATEGORY(LogPluginITLightning);

const TCHAR* GetITLGameMode(bool ForINISection)
{
	if (GIsEditor)
	{
		return ForINISection ? TEXT("Editor") : TEXT("editor");
	}
	else if (IsRunningCommandlet())
	{
		return ForINISection ? TEXT("Commandlet") : TEXT("commandlet");
	}
	else if (IsRunningDedicatedServer())
	{
		return ForINISection ? TEXT("Server") : TEXT("server");
	}
	else
	{
		return ForINISection ? TEXT("Client") : TEXT("client");
	}
}

FString GetITLINISectionName()
{
	const TCHAR* GameMode = GetITLGameMode(true);
	return FString(ITL_CONFIG_SECTION_NAME).Append(GameMode);
}

FString GetITLLogFileName(TCHAR* LogTypeName)
{
	const TCHAR* GameMode = GetITLGameMode(false);
	FString Name = FString(TEXT("itlightning-"), FCString::Strlen(GameMode) + FCString::Strlen(LogTypeName) + FCString::Strlen(TEXT("-.log")));
	Name.Append(GameMode).Append("-").Append(LogTypeName).Append(".log");
	return Name;
}

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
			LogDevice = MakeUnique<FOutputDeviceFile>(*LogFilePath, /*bDisableBackup*/true, /*bAppendIfExists*/true, /*bCreateWriterLazily*/true, [](const TCHAR* AbsPathname) {});
		}
	}
};

FITLLogOutputDeviceInitializer& GetITLInternalGameLog()
{
	static FITLLogOutputDeviceInitializer Singleton;
	FString LogFileName = GetITLLogFileName(TEXT("run"));
	Singleton.InitLogDevice(*LogFileName);
	return Singleton;
}

FITLLogOutputDeviceInitializer& GetITLInternalOpsLog()
{
	static FITLLogOutputDeviceInitializer Singleton;
	FString LogFileName = GetITLLogFileName(TEXT("ops"));
	Singleton.InitLogDevice(*LogFileName);
	return Singleton;
}

void FitlightningModule::StartupModule()
{
	UE_LOG(LogPluginITLightning, Log, TEXT("Plugin compiled on %s %s"), TEXT(__DATE__), TEXT(__TIME__));
	if (GIsEditor)
	{
		// We must force date/times to be logged in UTC for consistency.
		// Inside of the itlightninginit module, it forces that setting even before config is loaded.
		FString DefaultEngineIniPath = FPaths::ProjectConfigDir() + TEXT("DefaultEngine.ini");
		FString CurrentLogTimesValue = GConfig->GetStr(TEXT("LogFiles"), TEXT("LogTimes"), DefaultEngineIniPath);
		if (CurrentLogTimesValue != TEXT("UTC")) {
			UE_LOG(LogPluginITLightning, Warning, TEXT("Changing DefaultEngine.ini so [LogFiles]LogTimes=UTC"));
			GConfig->SetString(TEXT("LogFiles"), TEXT("LogTimes"), TEXT("UTC"), DefaultEngineIniPath);
		}
	}

	if (LoggingActive)
	{
		return;
	}

	LoadSettings();
	if (SettingAgentID.IsEmpty() || SettingAuthToken.IsEmpty())
	{
		UE_LOG(LogPluginITLightning, Log, TEXT("Not yet configured for this game mode. In DefaultEngine.ini section %s configure AgentID and AuthToken to enable. Consider using a different agent for Editor vs Client vs Server mode."), *GetITLINISectionName());
		return;
	}

	float DiceRoll = FMath::FRandRange(0.0, 100.0);
	LoggingActive = DiceRoll < SettingActivationPercent;
	if (LoggingActive)
	{
		// Log all messages to the internal game log, which we will then read from the file as we push log data to the cloud
		GLog->AddOutputDevice(GetITLInternalGameLog().LogDevice.Get());
		LoggingActive = true;
	}
	UE_LOG(LogPluginITLightning, Log, TEXT("Starting up. GameMode=%s, AgentID=%s, ActivationPercent=%lf, DiceRoll=%f, Activated=%s"), GetITLGameMode(true), *SettingAgentID, SettingActivationPercent, DiceRoll, LoggingActive ? TEXT("yes") : TEXT("no"));
}

void FitlightningModule::ShutdownModule()
{
	if (LoggingActive)
	{
		UE_LOG(LogPluginITLightning, Log, TEXT("Shutting down and flushing logs..."));
		GLog->Flush();
		// TODO: try to flush pending logs to the cloud. If successful, then we should purge the itl game log and delete the progress marker to 0.
		// (fully flushed logs should not leave behind the itlightning log files). That way, on a crash, we'll know we need to continue where we left off.
		UE_LOG(LogPluginITLightning, Log, TEXT("Shutdown."));
		LoggingActive = false;
	}
}

void FitlightningModule::LoadSettings()
{
	FString Section = GetITLINISectionName();
	SettingAgentID = GConfig->GetStr(*Section, TEXT("AgentID"), GEngineIni);
	SettingAgentID.TrimStartAndEndInline();
	SettingAuthToken = GConfig->GetStr(*Section, TEXT("AuthToken"), GEngineIni);
	SettingAuthToken.TrimStartAndEndInline();
	FString StringActivationPercent;
	GConfig->GetString(*Section, TEXT("ActivationPercent"), StringActivationPercent, GEngineIni);
	StringActivationPercent.TrimStartAndEndInline();
	if (!GConfig->GetDouble(*Section, TEXT("ActivationPercent"), SettingActivationPercent, GEngineIni))
	{
		SettingActivationPercent = 100.0;
	}
	else
	{
		// If it was an empty string, treat as 100%
		if (StringActivationPercent.IsEmpty())
		{
			SettingActivationPercent = 100.0;
		}
	}
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FitlightningModule, itlightning)
