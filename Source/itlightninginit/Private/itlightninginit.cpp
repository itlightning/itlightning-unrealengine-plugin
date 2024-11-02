// Copyright (C) 2024 IT Lightning, LLC. All rights reserved.
// Licensed software - see LICENSE

#include "itlightninginit.h"
#include <CoreGlobals.h>

#define LOCTEXT_NAMESPACE "FitlightninginitModule"

void FitlightninginitModule::StartupModule()
{
	// Don't change the global -- we are not capturing log events before the other module starts up anyway
	// GPrintLogTimes = ELogTimes::UTC;
	/**** Take care of this only once the configuration has loaded and we can detect the INI setting value there, etc.
	static IConsoleVariable* ICVar = IConsoleManager::Get().FindConsoleVariable(TEXT("log.Timestamp"), false);
	if (ICVar)
	{
		// Has to be either Local or UTC, force UTC if needed
		ELogTimes::Type CurrentValue = (ELogTimes::Type)ICVar->GetInt();
		if (CurrentValue != ELogTimes::UTC && CurrentValue != ELogTimes::Local)
		{
			UE_LOG(LogInit, Warning, TEXT("ITLightningPlugin: log.Timestamp not set to either Local or UTC; forcing to UTC"));
			ICVar->Set((int)ELogTimes::UTC, ECVF_SetByCode);
		}
	}
	***/
}

void FitlightninginitModule::ShutdownModule()
{
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FitlightninginitModule, itlightninginit)
