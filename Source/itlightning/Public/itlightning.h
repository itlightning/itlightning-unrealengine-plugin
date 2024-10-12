// Copyright (C) 2024 IT Lightning, LLC. All rights reserved.
// Licensed under the MIT license - see LICENSE

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

#define ITL_CONFIG_SECTION_NAME TEXT("ITLightning")

DECLARE_LOG_CATEGORY_EXTERN(LogPluginITLightning, Log, All);

class FitlightningModule : public IModuleInterface
{

public:
	FitlightningModule() : LoggingActive(false) {}

	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	/** Settings */
	void LoadSettings();

	bool LoggingActive;
	FString SettingAgentID;
	FString SettingAuthToken;
};
