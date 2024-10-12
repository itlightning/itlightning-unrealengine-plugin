// Copyright (C) 2024 IT Lightning, LLC. All rights reserved.
// Licensed under the MIT license - see LICENSE.md

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

#define ITL_CONFIG_SECTION_NAME TEXT("ITLightning")

DECLARE_LOG_CATEGORY_EXTERN(LogPluginITLightning, Log, All);

//UCLASS(Config=Game)
class FitlightningModule : public IModuleInterface
{

public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
