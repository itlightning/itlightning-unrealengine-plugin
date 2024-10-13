// Copyright (C) 2024 IT Lightning, LLC. All rights reserved.
// Licensed under the MIT license - see LICENSE

#include "itlightninginit.h"
#include <CoreGlobals.h>

#define LOCTEXT_NAMESPACE "FitlightninginitModule"

void FitlightninginitModule::StartupModule()
{
	GPrintLogTimes = ELogTimes::UTC;
	static IConsoleVariable* ICVar = IConsoleManager::Get().FindConsoleVariable(TEXT("log.Timestamp"), false);
	if (ICVar)
	{
		ICVar->Set((int)ELogTimes::UTC, ECVF_SetByCode);
	}
}

void FitlightninginitModule::ShutdownModule()
{
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FitlightninginitModule, itlightninginit)
