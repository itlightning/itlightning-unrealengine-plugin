// Copyright (C) 2024-2025 IT Lightning, LLC. All rights reserved.
// Licensed software - see LICENSE

#include "sparklogs.h"
#include "GenericPlatform/GenericPlatformOutputDevices.h"
#include "Misc/OutputDeviceFile.h"
#include "ISettingsModule.h"
#include "HAL/ThreadManager.h"

/*
#if UE_BUILD_SHIPPING
	#include "Compression/lz4.h"
#else
	#include "Trace/LZ4/lz4.c.inl"
#endif
*/
#define LZ4_NAMESPACE ITLLZ4
#include "Trace/LZ4/lz4.c.inl"
#undef LZ4_NAMESPACE


#define LOCTEXT_NAMESPACE "FsparklogsModule"

DEFINE_LOG_CATEGORY(LogPluginSparkLogs);

// =============== Globals ===============================================================================

constexpr int GMaxLineLength = 16 * 1024;

static uint8 UTF8ByteOrderMark[3] = {0xEF, 0xBB, 0xBF};

#if !NO_LOGGING
const FName SparkLogsCategoryName(LogPluginSparkLogs.GetCategoryName());
#else
const FName SparkLogsCategoryName(TEXT("LogPluginSparkLogs"));
#endif

FString ITLConvertUTF8(const void* Data, int Len)
{
	FUTF8ToTCHAR Converter((const ANSICHAR*)(Data), Len);
	return FString(Converter.Length(), Converter.Get());
}

const TCHAR* GetITLLaunchConfiguration(bool ForINISection)
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

FString GetITLINISettingPrefix()
{
	const TCHAR* LaunchConfiguration = GetITLLaunchConfiguration(true);
	return FString(LaunchConfiguration);
}

FString GetITLLogFileName(const TCHAR* LogTypeName)
{
	const TCHAR* LaunchConfiguration = GetITLLaunchConfiguration(false);
	FString Name = FString(TEXT("sparklogs-"), FCString::Strlen(LaunchConfiguration) + FCString::Strlen(LogTypeName) + FCString::Strlen(TEXT("-.log")));
	Name.Append(LaunchConfiguration).Append(TEXT("-")).Append(LogTypeName).Append(TEXT(".log"));
	return Name;
}

FString GetITLPluginStateFilename()
{
	const TCHAR* LaunchConfiguration = GetITLLaunchConfiguration(false);
	FString Name = FString(TEXT("sparklogs-"), FCString::Strlen(LaunchConfiguration) + FCString::Strlen(TEXT("-state.ini")));
	Name.Append(LaunchConfiguration).Append(TEXT("-state.ini"));
	return Name;
}

class FITLLogOutputDeviceInitializer
{
public:
	TUniquePtr<FOutputDeviceFile> LogDevice;
	FString LogFilePath;
	bool InitLogDevice(const TCHAR* Filename)
	{
		if (!LogDevice)
		{
			FString ParentDir = FPaths::GetPath(FPaths::ConvertRelativePathToFull(FGenericPlatformOutputDevices::GetAbsoluteLogFilename()));
			LogFilePath = FPaths::Combine(ParentDir, Filename);
			LogDevice = MakeUnique<FOutputDeviceFile>(*LogFilePath, /*bDisableBackup*/true, /*bAppendIfExists*/true, /*bCreateWriterLazily*/true, [](const TCHAR* AbsPathname) {});
			return true;
		}
		else
		{
			return false;
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
	if (Singleton.InitLogDevice(*LogFileName))
	{
		// The ops log should only contain logs about this plugin itself
		Singleton.LogDevice->IncludeCategory(SparkLogsCategoryName);
	}
	return Singleton;
}

bool ITLCompressData(ITLCompressionMode Mode, const uint8* InData, int InDataLen, TArray<uint8>& OutData)
{
	int32 CompressedBufSize = 0;
	int CompressedSize = 0;
	switch (Mode)
	{
	case ITLCompressionMode::LZ4:
		if (InDataLen > LZ4_MAX_INPUT_SIZE)
		{
			return false;
		}
		CompressedBufSize = (int32)ITLLZ4::LZ4_compressBound(InDataLen);
		OutData.SetNumUninitialized(CompressedBufSize, false);
		if (InDataLen <= 0)
		{
			// no-op
			return true;
		}
		CompressedSize = ITLLZ4::LZ4_compress_default((const char*)InData, (char*)OutData.GetData(), InDataLen, CompressedBufSize);
		if (CompressedSize <= 0)
		{
			return false;
		}
		OutData.SetNumUninitialized(CompressedSize, false);
		return true;
	case ITLCompressionMode::None:
		OutData.SetNumUninitialized(0, false);
		OutData.Append(InData, (int32)InDataLen);
		return true;
	default:
		return false;
	}
}

bool ITLDecompressData(ITLCompressionMode Mode, const uint8* InData, int InDataLen, int InOriginalDataLen, TArray<uint8>& OutData)
{
	int DecompressedBytes = 0;
	switch (Mode)
	{
	case ITLCompressionMode::LZ4:
		OutData.SetNumUninitialized(InOriginalDataLen, false);
		if (InOriginalDataLen <= 0)
		{
			// no-op
			return true;
		}
		DecompressedBytes = ITLLZ4::LZ4_decompress_safe((const char*)InData, (char*)OutData.GetData(), InDataLen, InOriginalDataLen);
		if (DecompressedBytes < 0)
		{
			return false;
		}
		OutData.SetNumUninitialized(DecompressedBytes, false);
		return true;
	case ITLCompressionMode::None:
		OutData.SetNumUninitialized(0, false);
		OutData.Append(InData, (int32)InDataLen);
		return true;
	default:
		return false;
	}
}

SPARKLOGS_API FString ITLGenerateRandomAlphaNumID(int Length)
{
	const FString Charset = TEXT("abcdefghijklmnopqrstuvwxyz0123456789");
	FString Result;
	Result.Reserve(Length);
	for (int i = 0; i < Length; ++i)
	{
		Result.AppendChar(Charset[FMath::RandRange(0, Charset.Len() - 1)]);
	}
	return Result;
}

// =============== FsparklogsSettings ===============================================================================

const TCHAR* FsparklogsSettings::PluginStateSection = TEXT("PluginState");

FsparklogsSettings::FsparklogsSettings()
	: RequestTimeoutSecs(DefaultRequestTimeoutSecs)
	, ActivationPercentage(DefaultActivationPercentage)
	, BytesPerRequest(DefaultBytesPerRequest)
	, ProcessingIntervalSecs(DefaultProcessingIntervalSecs)
	, RetryIntervalSecs(DefaultRetryIntervalSecs)
	, IncludeCommonMetadata(DefaultIncludeCommonMetadata)
	, DebugLogRequests(DefaultDebugLogRequests)
	, AutoStart(DefaultAutoStart)
	, CompressionMode(ITLCompressionMode::Default)
	, AddRandomGameInstanceID(DefaultAddRandomGameInstanceID)
	, StressTestGenerateIntervalSecs(0.0)
	, StressTestNumEntriesPerTick(0)
{
}

FString FsparklogsSettings::GetEffectiveHttpEndpointURI(const TCHAR* OverrideHTTPEndpointURI)
{
	CloudRegion.TrimStartAndEndInline();
	HttpEndpointURI.TrimStartAndEndInline();
	if (OverrideHTTPEndpointURI != NULL && FPlatformString::Strlen(OverrideHTTPEndpointURI) > 0)
	{
		return FString(OverrideHTTPEndpointURI);
	}
	if (HttpEndpointURI.Len() > 0)
	{
		return HttpEndpointURI;
	}
	FString CloudRegionLower = CloudRegion.ToLower();
	if (CloudRegionLower == TEXT("local"))
	{
		// Send to the local DEBUG container
		return TEXT("http://localhost:8082/ingest/v1");
	}
	else if (!CloudRegionLower.IsEmpty())
	{
		return FString::Format(TEXT("https://ingest-{0}.engine.sparklogs.app/ingest/v1"), { CloudRegionLower });
	}
	else
	{
		return FString();
	}
}

void FsparklogsSettings::LoadSettings()
{
	FString Section = ITL_CONFIG_SECTION_NAME;
	FString SettingPrefix = GetITLINISettingPrefix();

	CloudRegion = GConfig->GetStr(*Section, *(SettingPrefix + TEXT("CloudRegion")), GEngineIni);
	HttpEndpointURI = GConfig->GetStr(*Section, *(SettingPrefix + TEXT("HTTPEndpointURI")), GEngineIni);
	if (!GConfig->GetDouble(*Section, *(SettingPrefix + TEXT("RequestTimeoutSecs")), RequestTimeoutSecs, GEngineIni))
	{
		RequestTimeoutSecs = DefaultRequestTimeoutSecs;
	}

	AgentID = GConfig->GetStr(*Section, *(SettingPrefix + TEXT("AgentID")), GEngineIni);
	AgentAuthToken = GConfig->GetStr(*Section, *(SettingPrefix + TEXT("AgentAuthToken")), GEngineIni);
	HttpAuthorizationHeaderValue = GConfig->GetStr(*Section, *(SettingPrefix + TEXT("HTTPAuthorizationHeaderValue")), GEngineIni);
	
	FString StringActivationPercentage;
	GConfig->GetString(*Section, *(SettingPrefix + TEXT("ActivationPercentage")), StringActivationPercentage, GEngineIni);
	StringActivationPercentage.TrimStartAndEndInline();
	if (!GConfig->GetDouble(*Section, *(SettingPrefix + TEXT("ActivationPercentage")), ActivationPercentage, GEngineIni))
	{
		ActivationPercentage = DefaultActivationPercentage;
	}
	else
	{
		// If it was an empty string, treat as the default
		if (StringActivationPercentage.IsEmpty())
		{
			ActivationPercentage = DefaultActivationPercentage;
		}
	}

	if (!GConfig->GetInt(*Section, *(SettingPrefix + TEXT("BytesPerRequest")), BytesPerRequest, GEngineIni))
	{
		BytesPerRequest = DefaultBytesPerRequest;
	}
	if (!GConfig->GetDouble(*Section, *(SettingPrefix + TEXT("ProcessingIntervalSecs")), ProcessingIntervalSecs, GEngineIni))
	{
		ProcessingIntervalSecs = DefaultProcessingIntervalSecs;
	}
	if (!GConfig->GetDouble(*Section, *(SettingPrefix + TEXT("RetryIntervalSecs")), RetryIntervalSecs, GEngineIni))
	{
		RetryIntervalSecs = DefaultRetryIntervalSecs;
	}

	if (!GConfig->GetBool(*Section, *(SettingPrefix + TEXT("IncludeCommonMetadata")), IncludeCommonMetadata, GEngineIni))
	{
		IncludeCommonMetadata = DefaultIncludeCommonMetadata;
	}
	if (!GConfig->GetBool(*Section, *(SettingPrefix + TEXT("DebugLogRequests")), DebugLogRequests, GEngineIni))
	{
		DebugLogRequests = DefaultDebugLogRequests;
	}
	if (!GConfig->GetBool(*Section, *(SettingPrefix + TEXT("AutoStart")), AutoStart, GEngineIni))
	{
		AutoStart = DefaultAutoStart;
	}
	if (!GConfig->GetBool(*Section, *(SettingPrefix + TEXT("AddRandomGameInstanceID")), AddRandomGameInstanceID, GEngineIni))
	{
		AddRandomGameInstanceID = DefaultAddRandomGameInstanceID;
	}

	FString CompressionModeStr = GConfig->GetStr(*Section, *(SettingPrefix + TEXT("CompressionMode")), GEngineIni).ToLower();
	if (CompressionModeStr == TEXT("lz4"))
	{
		CompressionMode = ITLCompressionMode::LZ4;
	}
	else if (CompressionModeStr == TEXT("none"))
	{
		CompressionMode = ITLCompressionMode::None;
	}
	else
	{
		if (CompressionModeStr.Len() > 0)
		{
			UE_LOG(LogPluginSparkLogs, Warning, TEXT("Unknown compression_mode=%s, using default mode instead..."), *CompressionModeStr);
		}
		CompressionMode = ITLCompressionMode::Default;
	}

	if (!GConfig->GetDouble(*Section, *(SettingPrefix + TEXT("StressTestGenerateIntervalSecs")), StressTestGenerateIntervalSecs, GEngineIni))
	{
		StressTestGenerateIntervalSecs = 0.0;
	}
	if (!GConfig->GetInt(*Section, *(SettingPrefix + TEXT("StressTestNumEntriesPerTick")), StressTestNumEntriesPerTick, GEngineIni))
	{
		StressTestNumEntriesPerTick = 0;
	}

	EnforceConstraints();
}

void FsparklogsSettings::EnforceConstraints()
{
	AgentID.TrimStartAndEndInline();
	AgentAuthToken.TrimStartAndEndInline();

	if (RequestTimeoutSecs < MinRequestTimeoutSecs)
	{
		RequestTimeoutSecs = MinRequestTimeoutSecs;
	}
	if (RequestTimeoutSecs > MaxRequestTimeoutSecs)
	{
		RequestTimeoutSecs = MaxRequestTimeoutSecs;
	}
	if (BytesPerRequest < MinBytesPerRequest)
	{
		BytesPerRequest = MinBytesPerRequest;
	}
	if (BytesPerRequest > MaxBytesPerRequest)
	{
		BytesPerRequest = MaxBytesPerRequest;
	}
	if (ProcessingIntervalSecs < MinProcessingIntervalSecs)
	{
		ProcessingIntervalSecs = MinProcessingIntervalSecs;
	}
	if (RetryIntervalSecs < MinRetryIntervalSecs)
	{
		RetryIntervalSecs = MinRetryIntervalSecs;
	}
	if (RetryIntervalSecs > MaxRetryIntervalSecs)
	{
		RetryIntervalSecs = MaxRetryIntervalSecs;
	}
	if (StressTestGenerateIntervalSecs > 0 && StressTestNumEntriesPerTick < 1)
	{
		StressTestNumEntriesPerTick = 1;
	}
}

// =============== FsparklogsWriteNDJSONPayloadProcessor ===============================================================================

FsparklogsWriteNDJSONPayloadProcessor::FsparklogsWriteNDJSONPayloadProcessor(FString InOutputFilePath) : OutputFilePath(InOutputFilePath) { }

bool FsparklogsWriteNDJSONPayloadProcessor::ProcessPayload(TArray<uint8>& JSONPayloadInUTF8, int PayloadLen, int OriginalPayloadLen, ITLCompressionMode CompressionMode, FsparklogsReadAndStreamToCloud* Streamer)
{
	TUniquePtr<IFileHandle> DebugJSONWriter;
	DebugJSONWriter.Reset(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*OutputFilePath, true, true));
	if (DebugJSONWriter == nullptr)
	{
		return false;
	}
	TArray<uint8> DecompressedData;
	if (!ITLDecompressData(CompressionMode, JSONPayloadInUTF8.GetData(), PayloadLen, OriginalPayloadLen, DecompressedData))
	{
		UE_LOG(LogPluginSparkLogs, Warning, TEXT("WriteNDJSONPayloadProcessor: failed to decompress data in payload: mode=%d, len=%d, original_len=%d"), (int)CompressionMode, PayloadLen, OriginalPayloadLen);
		return false;
	}
	if (!DebugJSONWriter->Write((const uint8*)DecompressedData.GetData(), DecompressedData.Num())
		|| !DebugJSONWriter->Write((const uint8*)("\r\n"), 2)
		|| !DebugJSONWriter->Flush())
	{
		return false;
	}
	DebugJSONWriter.Reset();
	return true;
}

// =============== FsparklogsWriteHTTPPayloadProcessor ===============================================================================

FsparklogsWriteHTTPPayloadProcessor::FsparklogsWriteHTTPPayloadProcessor(const TCHAR* InEndpointURI, const TCHAR* InAuthorizationHeader, double InTimeoutSecs, bool InLogRequests)
	: EndpointURI(InEndpointURI)
	, AuthorizationHeader(InAuthorizationHeader)
	, LogRequests(InLogRequests)
{
	SetTimeoutSecs(InTimeoutSecs);
}

void FsparklogsWriteHTTPPayloadProcessor::SetTimeoutSecs(double InTimeoutSecs)
{
	TimeoutMillisec.Set((int32)(InTimeoutSecs * 1000.0));
}

bool FsparklogsWriteHTTPPayloadProcessor::ProcessPayload(TArray<uint8>& JSONPayloadInUTF8, int PayloadLen, int OriginalPayloadLen, ITLCompressionMode CompressionMode, FsparklogsReadAndStreamToCloud* Streamer)
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_FsparklogsWriteHTTPPayloadProcessor_ProcessPayload);
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("HTTPPayloadProcessor::ProcessPayload|BEGIN"));
	if (LogRequests)
	{
		UE_LOG(LogPluginSparkLogs, Log, TEXT("HTTPPayloadProcessor::ProcessPayload: BEGIN: len=%d, original_len=%d, timeout_millisec=%d"), PayloadLen, OriginalPayloadLen, (int)(TimeoutMillisec.GetValue()));
	}
	
	FThreadSafeBool RequestEnded(false);
	FThreadSafeBool RequestSucceeded(false);
	FThreadSafeBool RetryableFailure(true);
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = FHttpModule::Get().CreateRequest();
	HttpRequest->SetURL(*EndpointURI);
	HttpRequest->SetVerb(TEXT("POST"));
	SetHTTPTimezoneHeader(HttpRequest);
	HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json; charset=UTF-8"));
	HttpRequest->SetHeader(TEXT("Authorization"), *AuthorizationHeader);
	HttpRequest->SetTimeout((double)(TimeoutMillisec.GetValue()) / 1000.0);
	switch (CompressionMode)
	{
	case ITLCompressionMode::LZ4:
		HttpRequest->SetHeader(TEXT("Content-Encoding"), TEXT("lz4-block"));
		HttpRequest->SetHeader(TEXT("X-Original-Content-Length"), FString::FromInt(OriginalPayloadLen));
		break;
	case ITLCompressionMode::None:
		// no special header to set
		break;
	default:
		UE_LOG(LogPluginSparkLogs, Log, TEXT("HTTPPayloadProcessor::ProcessPayload: unknown compression mode %d"), (int)CompressionMode);
		return false;
	}
	HttpRequest->SetContent(JSONPayloadInUTF8);
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("HTTPPayloadProcessor::ProcessPayload|Headers and data prepared"));

	HttpRequest->OnProcessRequestComplete().BindLambda([&](FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
		{
			ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("HTTPPayloadProcessor::ProcessPayload|OnProcessRequestComplete|BEGIN"));
			if (LogRequests)
			{
				if (Response.IsValid())
				{
					UE_LOG(LogPluginSparkLogs, Log, TEXT("HTTPPayloadProcessor::ProcessPayload: RequestComplete: successful=%d, http_status=%d"), bWasSuccessful ? 1 : 0, (int)(Response->GetResponseCode()));
				}
				else
				{
					UE_LOG(LogPluginSparkLogs, Log, TEXT("HTTPPayloadProcessor::ProcessPayload: RequestComplete: successful=%d, null_response_object"), bWasSuccessful ? 1 : 0);
				}
			}
			if (bWasSuccessful && Response.IsValid())
			{
				FString ResponseBody = Response->GetContentAsString();
				int32 ResponseCode = Response->GetResponseCode();
				if (EHttpResponseCodes::IsOk(ResponseCode))
				{
					RequestSucceeded.AtomicSet(true);
				}
				else if (EHttpResponseCodes::TooManyRequests == ResponseCode || ResponseCode >= EHttpResponseCodes::ServerError)
				{
					UE_LOG(LogPluginSparkLogs, Warning, TEXT("HTTPPayloadProcessor::ProcessPayload: Retryable HTTP response: status=%d, msg=%s"), (int)ResponseCode, *ResponseBody.TrimStartAndEnd());
					RequestSucceeded.AtomicSet(false);
					RetryableFailure.AtomicSet(true);
				}
				else if (EHttpResponseCodes::BadRequest == ResponseCode)
				{
					// Something about this input was unable to be processed -- drop this input and pretend success so we can continue, but warn about it
					UE_LOG(LogPluginSparkLogs, Warning, TEXT("HTTPPayloadProcessor::ProcessPayload: HTTP response indicates input cannot be processed. Will skip this payload! status=%d, msg=%s"), (int)ResponseCode, *ResponseBody.TrimStartAndEnd());
					RequestSucceeded.AtomicSet(true);
				}
				else
				{
					UE_LOG(LogPluginSparkLogs, Warning, TEXT("HTTPPayloadProcessor::ProcessPayload: Non-Retryable HTTP response: status=%d, msg=%s"), (int)ResponseCode, *ResponseBody.TrimStartAndEnd());
					RequestSucceeded.AtomicSet(false);
					RetryableFailure.AtomicSet(false);
				}
			}
			else
			{
				UE_LOG(LogPluginSparkLogs, Warning, TEXT("HTTPPayloadProcessor::ProcessPayload: General HTTP request failure; will retry; retry_seconds=%.3lf"), Streamer->WorkerGetRetrySecs());
				RequestSucceeded.AtomicSet(false);
				RetryableFailure.AtomicSet(true);
			}

			// Signal that the request has finished (success or failure)
			RequestEnded.AtomicSet(true);
			ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("HTTPPayloadProcessor::ProcessPayload|OnProcessRequestComplete|END|RequestEnded=%d"), RequestEnded ? 1 : 0);
		});

	// Start the HTTP request
	double StartTime = FPlatformTime::Seconds();
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("HTTPPayloadProcessor::ProcessPayload|Starting to process request at time=%.3lf"), StartTime);
	if (!HttpRequest->ProcessRequest())
	{
		UE_LOG(LogPluginSparkLogs, Warning, TEXT("HTTPPayloadProcessor::ProcessPayload: failed to initiate HttpRequest"));
		RequestSucceeded.AtomicSet(false);
		RetryableFailure.AtomicSet(true);
	}
	else
	{
		// Synchronously wait for the request to complete or fail
		SleepWaitingForHTTPRequest(HttpRequest, RequestEnded, RequestSucceeded, RetryableFailure, StartTime);
	}

	// If we had a non-retryable failure, then trigger this worker to stop
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("HTTPPayloadProcessor::ProcessPayload|After request finished|RequestSucceeded=%d|RetryableFailure=%d"), RequestSucceeded ? 1 : 0, RetryableFailure ? 1 : 0);
	if (!RequestSucceeded && !RetryableFailure)
	{
		if (Streamer != nullptr)
		{
			UE_LOG(LogPluginSparkLogs, Error, TEXT("HTTPPayloadProcessor::ProcessPayload: stopping log streaming service after non-retryable failure"));
			Streamer->Stop();
		}
	}

	if (LogRequests)
	{
		UE_LOG(LogPluginSparkLogs, Log, TEXT("HTTPPayloadProcessor::ProcessPayload: END: success=%d, can_retry=%d"), RequestSucceeded ? 1 : 0, RetryableFailure ? 1 : 0);
	}
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("HTTPPayloadProcessor::ProcessPayload|END|RequestSucceeded=%d|RetryableFailure=%d"), RequestSucceeded ? 1 : 0, RetryableFailure ? 1 : 0);
	return RequestSucceeded;
}

void FsparklogsWriteHTTPPayloadProcessor::SetHTTPTimezoneHeader(TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest)
{
	static FString TimezoneHeader(TEXT("X-Timezone"));
	static FString TimezoneHeaderValueUTC(TEXT("UTC"));
	if (GPrintLogTimes == ELogTimes::Local)
	{
		FTimespan LocalOffset = FDateTime::Now() - FDateTime::UtcNow();
		int32 TotalMinutes = FMath::RoundToInt(LocalOffset.GetTotalMinutes());
		int32 Hours = FMath::Abs(TotalMinutes) / 60;
		int32 Minutes = FMath::Abs(TotalMinutes) % 60;
		const TCHAR* Sign = (TotalMinutes >= 0) ? TEXT("+") : TEXT("-");
		FString TZOffset = FString::Printf(TEXT("UTC%s%02d:%02d"), Sign, Hours, Minutes);
		HttpRequest->SetHeader(TimezoneHeader, TZOffset);
	}
	else
	{
		// Assume UTC
		HttpRequest->SetHeader(TimezoneHeader, TimezoneHeaderValueUTC);
	}
}

bool FsparklogsWriteHTTPPayloadProcessor::SleepWaitingForHTTPRequest(TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest, FThreadSafeBool& RequestEnded, FThreadSafeBool& RequestSucceeded, FThreadSafeBool& RetryableFailure, double StartTime)
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_FsparklogsWriteHTTPPayloadProcessor_SleepWaitingForHTTPRequest);
	while (!RequestEnded)
	{
		ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("HTTPPayloadProcessor::ProcessPayload|In loop waiting for request to end|RequestEnded=%d"), RequestEnded ? 1 : 0);
		// TODO: support cancellation in the future if we need to
		double CurrentTime = FPlatformTime::Seconds();
		double Elapsed = CurrentTime - StartTime;
		// It's possible the timeout has shortened while we've been waiting, so always use the current timeout value
		double Timeout = (double)(TimeoutMillisec.GetValue()) / 1000.0;
		if (Elapsed > Timeout)
		{
			UE_LOG(LogPluginSparkLogs, Warning, TEXT("HTTPPayloadProcessor::ProcessPayload: Timed out after %.3lf seconds; will retry..."), Elapsed);
			HttpRequest->CancelRequest();
			RequestSucceeded.AtomicSet(false);
			RetryableFailure.AtomicSet(true);
			return false;
		}
		// Capture stats in this sleep to ensure it is counted as an idle scope...
		FPlatformProcess::Sleep(0.1f);
	}
	return true;
}

// =============== FsparklogsStressGenerator ===============================================================================

FsparklogsStressGenerator::FsparklogsStressGenerator(TSharedRef<FsparklogsSettings> InSettings)
	: Settings(InSettings)
	, Thread(nullptr)
{
	check(FPlatformProcess::SupportsMultithreading());
	FString ThreadName = TEXT("SparkLogs_StressGenerator");
	FPlatformAtomics::InterlockedExchangePtr((void**)&Thread, FRunnableThread::Create(this, *ThreadName, 0, TPri_BelowNormal));
}

FsparklogsStressGenerator::~FsparklogsStressGenerator()
{
	if (Thread)
	{
		delete Thread;
	}
	Thread = nullptr;
}

bool FsparklogsStressGenerator::Init()
{
	return true;
}

uint32 FsparklogsStressGenerator::Run()
{
	double StressTestGenerateIntervalSecs = Settings->StressTestGenerateIntervalSecs;
	int StressTestNumEntriesPerTick = Settings->StressTestNumEntriesPerTick;
	UE_LOG(LogPluginSparkLogs, Log, TEXT("FsparklogsStressGenerator starting. StressTestGenerateIntervalSecs=%.3lf, StressTestNumEntriesPerTick=%d"), StressTestGenerateIntervalSecs, StressTestNumEntriesPerTick);
	while (StopRequestCounter.GetValue() == 0)
	{
		for (int i = 0; i < StressTestNumEntriesPerTick; i++)
		{
			UE_LOG(LogEngine, Log, TEXT("FsparklogsStressGenerator|Stress test message is being generated at platform_time=%.3lf, iteration=%d, 12345678901234567890123456789012345678901234567890 1234567890123456789012345678901234567890123456 100 12345678901234567890123456789012345678901234567890 1234567890123456789012345678901234567890123456 200 12345678901234567890123456789012345678901234567890 1234567890123456789012345678901234567890123456 300 12345678901234567890123456789012345678901234567890 1234567890123456789012345678901234567890123456 400"), FPlatformTime::Seconds(), i);
		}
		FPlatformProcess::SleepNoStats(StressTestGenerateIntervalSecs);
	}
	UE_LOG(LogPluginSparkLogs, Log, TEXT("FsparklogsStressGenerator stopped..."));
	return 0;
}

void FsparklogsStressGenerator::Stop()
{
	UE_LOG(LogPluginSparkLogs, Log, TEXT("FsparklogsStressGenerator requesting stop..."));
	StopRequestCounter.Increment();
}

// =============== FsparklogsReadAndStreamToCloud ===============================================================================

const TCHAR* FsparklogsReadAndStreamToCloud::ProgressMarkerValue = TEXT("ShippedLogOffset");

void FsparklogsReadAndStreamToCloud::ComputeCommonEventJSON(bool IncludeCommonMetadata, TMap<FString, FString>* AdditionalAttributes)
{
	FString CommonEventJSON;

	if (IncludeCommonMetadata)
	{
		FString EffectiveComputerName;
		if (OverrideComputerName.IsEmpty())
		{
			EffectiveComputerName = FPlatformProcess::ComputerName();
		}
		else
		{
			EffectiveComputerName = OverrideComputerName;
		}

		CommonEventJSON.Appendf(TEXT("\"hostname\": %s, \"pid\": %d"), *EscapeJsonString(EffectiveComputerName), FPlatformProcess::GetCurrentProcessId());
		FString ProjectName = FApp::GetProjectName();
		if (ProjectName.Len() > 0 && ProjectName != "None")
		{
			CommonEventJSON.Appendf(TEXT(", \"app\": %s"), *EscapeJsonString(ProjectName));
		}

		if (Settings->AddRandomGameInstanceID)
		{
			FString GameInstanceID = ITLGenerateRandomAlphaNumID(16);
			CommonEventJSON.Appendf(TEXT(", \"game_instance_id\": %s"), *EscapeJsonString(GameInstanceID));
		}
	}

	if (nullptr != AdditionalAttributes)
	{
		for (const TPair<FString, FString>& Pair : *AdditionalAttributes)
		{
			if (!CommonEventJSON.IsEmpty())
			{
				CommonEventJSON.Append(TEXT(","));
			}
			CommonEventJSON.Appendf(TEXT("%s:%s"), *EscapeJsonString(Pair.Key), *EscapeJsonString(Pair.Value));
		}
	}

	if (!CommonEventJSON.IsEmpty())
	{
		UE_LOG(LogPluginSparkLogs, Log, TEXT("Common event JSON computed. unreal_engine_common_event_data={%s}"), *CommonEventJSON);
		int64 CommonEventJSONLen = FTCHARToUTF8_Convert::ConvertedLength(*CommonEventJSON, CommonEventJSON.Len());
		CommonEventJSONData.SetNum(0, false);
		CommonEventJSONData.AddUninitialized(CommonEventJSONLen);
		FTCHARToUTF8_Convert::Convert(CommonEventJSONData.GetData(), CommonEventJSONLen, *CommonEventJSON, CommonEventJSON.Len());
	}
}

FsparklogsReadAndStreamToCloud::FsparklogsReadAndStreamToCloud(const TCHAR* InSourceLogFile, TSharedRef<FsparklogsSettings> InSettings, TSharedRef<IsparklogsPayloadProcessor> InPayloadProcessor, int InMaxLineLength, const TCHAR* InOverrideComputerName, TMap<FString, FString>* AdditionalAttributes)
	: Settings(InSettings)
	, PayloadProcessor(InPayloadProcessor)
	, SourceLogFile(InSourceLogFile)
	, MaxLineLength(InMaxLineLength)
	, OverrideComputerName(InOverrideComputerName == nullptr ? TEXT("") : InOverrideComputerName)
	, Thread(nullptr)
	, WorkerShippedLogOffset(0)
	, WorkerMinNextFlushPlatformTime(0)
	, WorkerNumConsecutiveFlushFailures(0)
	, WorkerLastFailedFlushPayloadSize(0)
{
	ProgressMarkerPath = FPaths::Combine(FPaths::GetPath(InSourceLogFile), GetITLPluginStateFilename());
	ComputeCommonEventJSON(Settings->IncludeCommonMetadata, AdditionalAttributes);

	WorkerBuffer.AddUninitialized(Settings->BytesPerRequest);
	int BufferSize = Settings->BytesPerRequest + 4096 + (Settings->BytesPerRequest / 10);
	WorkerNextPayload.AddUninitialized(BufferSize);
	WorkerNextEncodedPayload.AddUninitialized(BufferSize);
	check(MaxLineLength > 0);
	check(FPlatformProcess::SupportsMultithreading());
	FString ThreadName = FString::Printf(TEXT("SparkLogs_Reader_%s"), *FPaths::GetBaseFilename(InSourceLogFile));
	FPlatformAtomics::InterlockedExchangePtr((void**)&Thread, FRunnableThread::Create(this, *ThreadName, 0, TPri_BelowNormal));
}

FsparklogsReadAndStreamToCloud::~FsparklogsReadAndStreamToCloud()
{
	if (Thread)
	{
		delete Thread;
	}
	Thread = nullptr;
}

bool FsparklogsReadAndStreamToCloud::Init()
{
	return true;
}

uint32 FsparklogsReadAndStreamToCloud::Run()
{
	WorkerFullyCleanedUp.AtomicSet(false);
	ReadProgressMarker(WorkerShippedLogOffset);
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|Run|BEGIN|WorkerShippedLogOffset=%d"), (int)WorkerShippedLogOffset);
	// A pending flush will be processed before stopping
	while (StopRequestCounter.GetValue() == 0 || FlushRequestCounter.GetValue() > 0)
	{
		ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|Run|In loop|WorkerLastFlushFailed=%d|FlushRequestCounter=%d"), WorkerLastFlushFailed ? 1 : 0, (int)FlushRequestCounter.GetValue());
		// Only allow manual flushes if we are not in a retry delay because the last operation failed.
		if (WorkerLastFlushFailed == false && FlushRequestCounter.GetValue() > 0)
		{
			int32 NewValue = FlushRequestCounter.Decrement();
			ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|Run|Manual flush requested|FlushRequestCounter=%d"), (int)NewValue);
			WorkerDoFlush();
		}
		else if (FPlatformTime::Seconds() > WorkerMinNextFlushPlatformTime)
		{
			// If we are waiting on a manual flush, and the retry timer finally expired, it's OK to mark this attempt as processing it.
			if (FlushRequestCounter.GetValue() > 0)
			{
				int32 NewValue = FlushRequestCounter.Decrement();
				ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|Run|Manual flush requested after retry timer expired|FlushRequestCounter=%d"), (int)NewValue);
			}
			else
			{
				ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|Run|Periodic flush"));
			}
			WorkerDoFlush();
		}
		else
		{
			// More coarse-grained sleep, we don't need to wake up and do work very often
			FPlatformProcess::SleepNoStats(0.1f);
		}
	}
	WorkerFullyCleanedUp.AtomicSet(true);
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|Run|END"));
	return 0;
}

void FsparklogsReadAndStreamToCloud::Stop()
{
	int32 NewValue = StopRequestCounter.Increment();
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|Stop|StopRequestCounter=%d"), (int)NewValue);
}

bool FsparklogsReadAndStreamToCloud::FlushAndWait(int N, bool ClearRetryTimer, bool InitiateStop, bool OnMainGameThread, double TimeoutSec, bool& OutLastFlushProcessedEverything)
{
	OutLastFlushProcessedEverything = false;
	bool WasSuccessful = true;

	// If we've already requested a stop, a flush is impossible
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|FlushAndWait|StopRequestCounter=%d"), (int)StopRequestCounter.GetValue());
	if (StopRequestCounter.GetValue() > 0)
	{
		ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|FlushAndWait|stop already requested, exiting with false"));
		return false;
	}

	if (ClearRetryTimer)
	{
		ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|FlushAndWait|Clearing retry timer..."));
		WorkerLastFlushFailed.AtomicSet(false);
	}

	for (int i = 0; i < N; i++)
	{
		int StartFlushSuccessOpCounter = FlushSuccessOpCounter.GetValue();
		int StartFlushOpCounter = FlushOpCounter.GetValue();
		ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|FlushAndWait|Starting Loop|i=%d|N=%d|FlushSuccessOpCounter=%d|FlushOpCounter=%d"), (int)i, (int)N, (int)StartFlushSuccessOpCounter, (int)StartFlushOpCounter);
		FlushRequestCounter.Increment();
		// Last time around, we might initiate a stop
		if (InitiateStop && i == N-1)
		{
			ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|FlushAndWait|Initiating stop..."));
			Stop();
		}
		double StartTime = FPlatformTime::Seconds();
		double LastTime = StartTime;
		ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|FlushAndWait|Waiting for request to finish...|StartTime=%.3lf"), StartTime);
		while (FlushOpCounter.GetValue() == StartFlushOpCounter)
		{
			double Now = FPlatformTime::Seconds();
			if (Now - StartTime > TimeoutSec)
			{
				ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|FlushAndWait|Timed out, returning false"));
				return false;
			}
			if (OnMainGameThread)
			{
				// HTTP requests and other things won't be processed unless we tick
				double DeltaTime = Now - LastTime;
				FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
				FTicker::GetCoreTicker().Tick(DeltaTime);
				FThreadManager::Get().Tick();
				// NOTE: the game does not normally progress the frame count during shutdown, follow the same logic here
				// GFrameCounter++;
			}
			FPlatformProcess::SleepNoStats(OnMainGameThread ? 0.01f : 0.05f);
			LastTime = Now;
		}
		WasSuccessful = FlushSuccessOpCounter.GetValue() != StartFlushSuccessOpCounter;
		ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|FlushAndWait|Finished waiting for request|WasSuccessful=%d|FlushSuccessOpCounter=%d|FlushOpCounter=%d"), WasSuccessful ? 1 : 0, (int)FlushSuccessOpCounter.GetValue(), (int)FlushOpCounter.GetValue());
	}
	if (WasSuccessful)
	{
		OutLastFlushProcessedEverything = LastFlushProcessedEverything;
		ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|FlushAndWait|LastFlushProcessedEverything=%d"), OutLastFlushProcessedEverything ? 1 : 0);
	}
	if (InitiateStop) {
		// Wait for the worker to fully stop, up to the timeout
		double StartTime = FPlatformTime::Seconds();
		ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|FlushAndWait|Waiting for thread to stop...|StartTime=%.3lf"), StartTime);
		while (!WorkerFullyCleanedUp)
		{
			if (FPlatformTime::Seconds() - StartTime > TimeoutSec)
			{
				ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|FlushAndWait|Timed out waiting for thread to stop"));
				return false;
			}
			FPlatformProcess::SleepNoStats(0.01f);
		}
	}
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|FlushAndWait|END|WasSuccessful=%d"), WasSuccessful ? 1 : 0);
	return WasSuccessful;
}

bool FsparklogsReadAndStreamToCloud::ReadProgressMarker(int64& OutMarker)
{
	OutMarker = 0;
	double OutDouble = 0.0;
	if (IFileManager::Get().FileExists(*ProgressMarkerPath))
	{
		bool WasDisabled = GConfig->AreFileOperationsDisabled();
		GConfig->EnableFileOperations();
		bool Result = GConfig->GetDouble(FsparklogsSettings::PluginStateSection, ProgressMarkerValue, OutDouble, *ProgressMarkerPath);
		if (WasDisabled)
		{
			GConfig->DisableFileOperations();
		}
		if (!Result)
		{
			UE_LOG(LogPluginSparkLogs, Warning, TEXT("Failed to read progress marker from %s"), *ProgressMarkerPath);
			return false;
		}
	}
	// Precise to 52+ bits
	OutMarker = (int64)(OutDouble);
	return true;
}

bool FsparklogsReadAndStreamToCloud::WriteProgressMarker(int64 InMarker)
{
	// TODO: should we use the sqlite plugin instead, maybe it's not as much overhead as writing INI file each time?
	// Precise to 52+ bits
	bool WasDisabled = GConfig->AreFileOperationsDisabled();
	GConfig->EnableFileOperations();
	GConfig->SetDouble(FsparklogsSettings::PluginStateSection, ProgressMarkerValue, (double)(InMarker), *ProgressMarkerPath);
	GConfig->Flush(false, ProgressMarkerPath);
	if (WasDisabled)
	{
		GConfig->DisableFileOperations();
	}
	return true;
}

void FsparklogsReadAndStreamToCloud::DeleteProgressMarker()
{
	IFileManager::Get().Delete(*ProgressMarkerPath, false, true, false);
}

bool FindFirstByte(const uint8* Haystack, uint8 Needle, int MaxToSearch, int& OutIndex)
{
	OutIndex = -1;
	for (const uint8* RESTRICT Data = Haystack, *RESTRICT End = Data + MaxToSearch; Data != End; ++Data)
	{
		if (*Data == Needle)
		{
			OutIndex = static_cast<int>(Data - Haystack);
			return true;
		}
	}
	return false;
}

void AppendUTF8AsEscapedJsonString(TITLJSONStringBuilder& Builder, const ANSICHAR* String, int N)
{
	ANSICHAR ControlFormatBuf[16];
	Builder.Append('\"');
	for (const ANSICHAR* RESTRICT Data = String, *RESTRICT End = Data + N; Data != End; ++Data)
	{
		switch (*Data)
		{
		case '\"':
			Builder.Append("\\\"", 2 /* string length */);
			break;
		case '\b':
			Builder.Append("\\b", 2 /* string length */);
			break;
		case '\t':
			Builder.Append("\\t", 2 /* string length */);
			break;
		case '\n':
			Builder.Append("\\n", 2 /* string length */);
			break;
		case '\f':
			Builder.Append("\\f", 2 /* string length */);
			break;
		case '\r':
			Builder.Append("\\r", 2 /* string length */);
			break;
		case '\\':
			Builder.Append("\\\\", 2 /* string length */);
			break;
		default:
			// Any character 0x20 and above can be included as-is
			if ((uint8)(*Data) >= static_cast<UTF8CHAR>(0x20))
			{
				Builder.Append(*Data);
			}
			else
			{
				// Rare control character
				FCStringAnsi::Snprintf(ControlFormatBuf, sizeof(ControlFormatBuf), "\\u%04x", static_cast<int>(*Data));
				Builder.AppendAnsi(ControlFormatBuf);
			}
		}
	}
	Builder.Append('\"');
}

bool FsparklogsReadAndStreamToCloud::WorkerReadNextPayload(int& OutNumToRead, int64& OutEffectiveShippedLogOffset, int64& OutRemainingBytes)
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_FsparklogsReadAndStreamToCloud_WorkerReadNextPayload);

	OutEffectiveShippedLogOffset = WorkerShippedLogOffset;

	// Re-open the file. UE doesn't contain cross-platform class that can stay open and refresh the filesize OR to read up to N (but maybe less than N bytes).
	// The only solution and stay within UE class library is to just re-open the file every flush request. This is actually quite fast on modern platforms.
	TUniquePtr<IFileHandle> WorkerReader;
	WorkerReader.Reset(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*SourceLogFile, true));
	if (WorkerReader == nullptr)
	{
		UE_LOG(LogPluginSparkLogs, Warning, TEXT("STREAMER: Failed to open logfile='%s'"), *SourceLogFile);
		return false;
	}
	int64 FileSize = WorkerReader->Size();
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerReadNextPayload|opened log file|last_offset=%ld|current_file_size=%ld|logfile='%s'"), OutEffectiveShippedLogOffset, FileSize, *SourceLogFile);
	if (OutEffectiveShippedLogOffset > FileSize)
	{
		UE_LOG(LogPluginSparkLogs, Log, TEXT("STREAMER: Logfile reduced size, re-reading from start: new_size=%ld, previously_processed_to=%ld, logfile='%s'"), FileSize, OutEffectiveShippedLogOffset, *SourceLogFile);
		OutEffectiveShippedLogOffset = 0;
		// Don't force a retried read to use the same payload size as last time since the whole file has changed.
		WorkerLastFailedFlushPayloadSize = 0;
	}
	// Start at the last known shipped position, read as many bytes as possible up to the max buffer size, and capture log lines into a JSON payload
	WorkerReader->Seek(OutEffectiveShippedLogOffset);
	OutRemainingBytes = FileSize - OutEffectiveShippedLogOffset;
	OutNumToRead = (int)(FMath::Clamp<int64>(OutRemainingBytes, 0, (int64)(WorkerBuffer.Num())));
	if (WorkerLastFailedFlushPayloadSize > 0 && OutNumToRead > WorkerLastFailedFlushPayloadSize)
	{
		// Retried requests always use the same max payload size as last time,
		// so that any retry has the same data as last time and can be deduplicated in worst-case scenarios.
		// (e.g., an actual observed scenario where Unreal Engine HTTP plugin was sending requests successfully
		// but was not processing responses properly and instead timing them out...)
		OutNumToRead = WorkerLastFailedFlushPayloadSize;
	}
	if (OutNumToRead <= 0)
	{
		// We've read everything we possibly can already
		ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerReadNextPayload|Nothing more can be read|FileSize=%ld|EffectiveShippedLogOffset=%ld"), FileSize, OutEffectiveShippedLogOffset);
		return true;
	}

	uint8* BufferData = WorkerBuffer.GetData();
	if (!WorkerReader->Read(BufferData, OutNumToRead))
	{
		UE_LOG(LogPluginSparkLogs, Warning, TEXT("STREAMER: Failed to read data: offset=%ld, bytes=%ld, logfile='%s'"), OutEffectiveShippedLogOffset, OutNumToRead, *SourceLogFile);
		return false;
	}
#if ITL_INTERNAL_DEBUG_LOG_DATA == 1
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerReadNextPayload|read data into buffer|offset=%ld|data_len=%d|data=%s|logfile='%s'"), OutEffectiveShippedLogOffset, OutNumToRead, *ITLConvertUTF8(BufferData, OutNumToRead), *SourceLogFile);
#else
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerReadNextPayload|read data into buffer|offset=%ld|data_len=%d|logfile='%s'"), OutEffectiveShippedLogOffset, OutNumToRead, *SourceLogFile);
#endif
	return true;
}

bool FsparklogsReadAndStreamToCloud::WorkerBuildNextPayload(int NumToRead, int& OutCapturedOffset, int& OutNumCapturedLines)
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_FsparklogsReadAndStreamToCloud_WorkerBuildNextPayload);
	OutCapturedOffset = 0;
	const uint8* BufferData = WorkerBuffer.GetData();
	OutNumCapturedLines = 0;
	WorkerNextPayload.Reset();
	WorkerNextPayload.Append('[');
	int NextOffset = 0;
	while (NextOffset < NumToRead)
	{
		// Skip the UTF-8 byte order marker (always at the start of the file)
		if (0 == std::memcmp(BufferData + NextOffset, UTF8ByteOrderMark, sizeof(UTF8ByteOrderMark)))
		{
			ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerBuildNextPayload|skipping UTF8 BOM|offset_before=%d|offset_after=%d"), NextOffset, NextOffset + sizeof(UTF8ByteOrderMark));
			NextOffset += sizeof(UTF8ByteOrderMark);
			OutCapturedOffset = NextOffset;
			continue;
		}
		// We only process whole lines. See if we can find the next end of line character.
		int RemainingBytes = NumToRead - NextOffset;
		int NumToSearch = FMath::Min(RemainingBytes, MaxLineLength);
		int FoundIndex = 0;
		int ExtraToSkip = 1; // skip over the \n char
		bool HaveLine = FindFirstByte(BufferData + NextOffset, static_cast<uint8>('\n'), NumToSearch, FoundIndex);
		ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerBuildNextPayload|after newline search|NextOffset=%d|HaveLine=%d|NumToSearch=%d|FoundIndex=%d"), NextOffset, (int)HaveLine, NumToSearch, FoundIndex);
		if (!HaveLine && NumToSearch == MaxLineLength && RemainingBytes > NumToSearch)
		{
			// Even though we didn't find a line, break the line at the max length and process it
			// It's unsafe to break a line in the middle of a multi-byte UTF-8, so find a safe break point...
			ExtraToSkip = 0;
			FoundIndex = MaxLineLength - 1;
			ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerBuildNextPayload|no newline found, search for safe breakpoint|NextOffset=%d|FoundIndex=%d"), NextOffset, FoundIndex);
			while (FoundIndex > 0)
			{
				if (*(BufferData + NextOffset + FoundIndex) >= 0x80)
				{
					FoundIndex--;
				}
				else
				{
					// include this non-multi-byte character and break here
					FoundIndex++;
					break;
				}
			}
			HaveLine = true;
			ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerBuildNextPayload|found safe breakpoint|NextOffset=%d|FoundIndex=%d|ExtraToSkip=%d"), NextOffset, FoundIndex, ExtraToSkip);
		}
		if (!HaveLine)
		{
			// No more complete lines to process, this is enough for now
			ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerBuildNextPayload|no more lines to process, break"));
			break;
		}
		// Trim newlines control characters of any kind at the end
		while (FoundIndex > 0)
		{
			// We expect the FoundIndex to be the *first* non-newline character, and ExtraToSkip set to the number of newline chars to skip.
			// Check if the previous character is a newline character, and if so, skip capturing it.
			uint8 c = *(BufferData + NextOffset + FoundIndex - 1);
			if (c == '\n' || c == '\r')
			{
				ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerBuildNextPayload|character at NextOffset=%d, FoundIndex=%d is newline, will skip it"), NextOffset, FoundIndex);
				ExtraToSkip++;
				FoundIndex--;
			}
			else
			{
				break;
			}
		}
		ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerBuildNextPayload|line summary|NextOffset=%d|FoundIndex=%d|ExtraToSkip=%d"), NextOffset, FoundIndex, ExtraToSkip);
		// Skip blank lines without capturing anything
		if (FoundIndex <= 0)
		{
			ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerBuildNextPayload|skipping blank line..."));
			if (ExtraToSkip <= 0)
			{
				ExtraToSkip = 1;
			}
			NextOffset += ExtraToSkip;
			OutCapturedOffset = NextOffset;
			continue;
		}
		// Capture the data from (BufferData + NextOffset) to (BufferData + NextOffset + FoundIndex)
		// NOTE: the data in the logfile was already written in UTF-8 format
		if (OutNumCapturedLines > 0)
		{
			WorkerNextPayload.Append(',');
		}
		WorkerNextPayload.Append('{');
		if (CommonEventJSONData.Num() > 0)
		{
			WorkerNextPayload.Append((const ANSICHAR*)(CommonEventJSONData.GetData()), CommonEventJSONData.Num());
			WorkerNextPayload.Append(',');
		}
		WorkerNextPayload.Append("\"message\":", 10 /* length of `"message":` */);
		AppendUTF8AsEscapedJsonString(WorkerNextPayload, (const ANSICHAR*)(BufferData + NextOffset), FoundIndex);
#if ITL_INTERNAL_DEBUG_LOG_DATA == 1
		ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerBuildNextPayload|adding message to payload: %s"), *ITLConvertUTF8(BufferData + NextOffset, FoundIndex));
#endif
		WorkerNextPayload.Append('}');
		OutNumCapturedLines++;
		NextOffset += FoundIndex + ExtraToSkip;
		OutCapturedOffset = NextOffset;
	}
	WorkerNextPayload.Append(']');
	return true;
}

bool FsparklogsReadAndStreamToCloud::WorkerCompressPayload()
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_FsparklogsReadAndStreamToCloud_WorkerCompressPayload);
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerCompressPayload|Begin compressing payload"));
	bool Success = ITLCompressData(Settings->CompressionMode, (const uint8*)WorkerNextPayload.GetData(), WorkerNextPayload.Len(), WorkerNextEncodedPayload);
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerCompressPayload|Finish compressing payload|success=%d|original_len=%d|compressed_len=%d"), Success ? 1 : 0, (int)WorkerNextPayload.Len(), (int)WorkerNextEncodedPayload.Num());
	return Success;
}

bool FsparklogsReadAndStreamToCloud::WorkerInternalDoFlush(int64& OutNewShippedLogOffset, bool& OutFlushProcessedEverything)
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_FsparklogsReadAndStreamToCloud_WorkerInternalDoFlush);
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerInternalDoFlush|BEGIN"));
	OutNewShippedLogOffset = WorkerShippedLogOffset;
	OutFlushProcessedEverything = false;
	
	int NumToRead = 0;
	int64 EffectiveShippedLogOffset = WorkerShippedLogOffset, RemainingBytes;
	if (!WorkerReadNextPayload(NumToRead, EffectiveShippedLogOffset, RemainingBytes))
	{
		return false;
	}
	if (NumToRead <= 0)
	{
		// nothing more to read
		OutFlushProcessedEverything = true;
		return true;
	}
	
	int CapturedOffset = 0;
	int NumCapturedLines = 0;
	if (!WorkerBuildNextPayload(NumToRead, CapturedOffset, NumCapturedLines))
	{
		UE_LOG(LogPluginSparkLogs, Warning, TEXT("STREAMER: Failed to build payload: offset=%ld, payload_input_size=%d, logfile='%s'"), EffectiveShippedLogOffset, CapturedOffset, *SourceLogFile);
		return false;
	}

#if ITL_INTERNAL_DEBUG_LOG_DATA == 1
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerInternalDoFlush|payload is ready to process|offset=%ld|payload_input_size=%d|captured_lines=%d|data_len=%d|data=%s|logfile='%s'"),
		EffectiveShippedLogOffset, CapturedOffset, NumCapturedLines, WorkerNextPayload.Len(), *ITLConvertUTF8(WorkerNextPayload.GetData(), WorkerNextPayload.Len()), *SourceLogFile);
#else
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerInternalDoFlush|payload is ready to process|offset=%ld|payload_input_size=%d|captured_lines=%d|data_len=%d|logfile='%s'"),
		EffectiveShippedLogOffset, CapturedOffset, NumCapturedLines, WorkerNextPayload.Len(), *SourceLogFile);
#endif
	if (NumCapturedLines > 0)
	{
		if (!WorkerCompressPayload())
		{
			UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER: Failed to compress payload: mode=%d"), (int)Settings->CompressionMode);
			return false;
		}
		ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerInternalDoFlush|Begin processing payload"));
		if (!PayloadProcessor->ProcessPayload(WorkerNextEncodedPayload, WorkerNextEncodedPayload.Num(), WorkerNextPayload.Len(), Settings->CompressionMode, this))
		{
			UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER: Failed to process payload: offset=%ld, num_read=%d, payload_input_size=%d, logfile='%s'"), EffectiveShippedLogOffset, NumToRead, CapturedOffset, *SourceLogFile);
			WorkerLastFailedFlushPayloadSize = NumToRead;
			return false;
		}
		ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerInternalDoFlush|Finished processing payload|PayloadInputSize=%ld"), CapturedOffset);
	}
	int ProcessedOffset = CapturedOffset;

	// If we processed everything up until the end of the file, we captured everything we can.
	OutNewShippedLogOffset = EffectiveShippedLogOffset + ProcessedOffset;
	if ((int64)(ProcessedOffset) >= RemainingBytes)
	{
		OutFlushProcessedEverything = true;
	}
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerInternalDoFlush|END|FlushProcessedEverything=%d"), OutFlushProcessedEverything);
	return true;
}

bool FsparklogsReadAndStreamToCloud::WorkerDoFlush()
{
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerDoFlush|BEGIN"));
	int64 ShippedNewLogOffset = 0;
	bool FlushProcessedEverything = false;
	bool Result = WorkerInternalDoFlush(ShippedNewLogOffset, FlushProcessedEverything);
	if (!Result)
	{
		WorkerLastFlushFailed.AtomicSet(true);
		WorkerMinNextFlushPlatformTime = FPlatformTime::Seconds() + WorkerGetRetrySecs();
		LastFlushProcessedEverything.AtomicSet(false);
		// Increment this counter after the retry interval is calculated
		WorkerNumConsecutiveFlushFailures++;
		ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerDoFlush|internal flush failed|WorkerMinNextFlushPlatformTime=%.3lf|NumConsecutiveFlushFailures=%d"), WorkerMinNextFlushPlatformTime, WorkerNumConsecutiveFlushFailures);
	}
	else
	{
		WorkerLastFlushFailed.AtomicSet(false);
		WorkerNumConsecutiveFlushFailures = 0;
		WorkerLastFailedFlushPayloadSize = 0;
		WorkerShippedLogOffset = ShippedNewLogOffset;
		WriteProgressMarker(ShippedNewLogOffset);
		WorkerMinNextFlushPlatformTime = FPlatformTime::Seconds() + Settings->ProcessingIntervalSecs;
		LastFlushProcessedEverything.AtomicSet(FlushProcessedEverything);
		FlushSuccessOpCounter.Increment();
		ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerDoFlush|internal flush succeeded|ShippedNewLogOffset=%d|WorkerMinNextFlushPlatformTime=%.3lf|FlushProcessedEverything=%d"), (int)ShippedNewLogOffset, WorkerMinNextFlushPlatformTime, FlushProcessedEverything ? 1 : 0);
	}
	FlushOpCounter.Increment();
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerDoFlush|END|Result=%d"), Result ? 1 : 0);
	return Result;
}

double FsparklogsReadAndStreamToCloud::WorkerGetRetrySecs()
{
	double RetrySecs = Settings->RetryIntervalSecs * (WorkerNumConsecutiveFlushFailures + 1);
	if (RetrySecs > Settings->MaxRetryIntervalSecs)
	{
		RetrySecs = Settings->MaxRetryIntervalSecs;
	}
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerGetRetrySecs=%.3lf"), RetrySecs);
	return RetrySecs;
}

// =============== FsparklogsModule ===============================================================================

FsparklogsModule::FsparklogsModule()
	: LoggingActive(false)
	, Settings(new FsparklogsSettings())
{
}

void FsparklogsModule::StartupModule()
{
	FCoreDelegates::OnPostEngineInit.AddRaw(this, &FsparklogsModule::OnPostEngineInit);
	// TODO: does it matter if we are loaded later and miss a bunch of log entries during engine initialization?
	// TODO: Should run plugin earlier and check command line to determine if this is running in an editor with
	//       similar logic to FEngineLoop::PreInitPreStartupScreen [LaunchEngineLoop.cpp] (GIsEditor not available earlier).
	//       If we change it here, also change GetITLLaunchConfiguration.
	IConsoleVariable* ICVar = IConsoleManager::Get().FindConsoleVariable(TEXT("log.Timestamp"), false);
	if (GIsEditor)
	{
		// We must force date/times to be logged in either UTC or Local so that each log message contains a timestamp.
		FString DefaultEngineIniPath = FPaths::ProjectConfigDir() + TEXT("DefaultEngine.ini");
		FString CurrentLogTimesValue = GConfig->GetStr(TEXT("LogFiles"), TEXT("LogTimes"), DefaultEngineIniPath).TrimStartAndEnd();
		if (CurrentLogTimesValue.Len() > 0 && CurrentLogTimesValue != TEXT("UTC") && CurrentLogTimesValue != TEXT("Local")) {
			UE_LOG(LogPluginSparkLogs, Warning, TEXT("Timestamps in log messages are required (LogTimes must be UTC or Local). Changing DefaultEngine.ini so [LogFiles]LogTimes=UTC"));
			GConfig->SetString(TEXT("LogFiles"), TEXT("LogTimes"), TEXT("UTC"), DefaultEngineIniPath);
			GPrintLogTimes = ELogTimes::UTC;
			if (ICVar)
			{
				ICVar->Set((int)ELogTimes::UTC, ECVF_SetByCode);
			}
		}
	}
	else
	{
		if (ICVar)
		{
			// Has to be either Local or UTC, force UTC if needed
			ELogTimes::Type CurrentValue = (ELogTimes::Type)ICVar->GetInt();
			if (CurrentValue != ELogTimes::UTC && CurrentValue != ELogTimes::Local)
			{
				UE_LOG(LogPluginSparkLogs, Warning, TEXT("SparkLogsPlugin: log.Timestamp not set to either Local or UTC; forcing to UTC"));
				ICVar->Set((int)ELogTimes::UTC, ECVF_SetByCode);
			}
		}
	}

	Settings->LoadSettings();
	if (Settings->AutoStart)
	{
		StartShippingEngine(NULL, NULL, NULL, NULL, NULL, NULL, false);
	}
	else
	{
		UE_LOG(LogPluginSparkLogs, Log, TEXT("AutoStart is disabled. Waiting for call to FsparklogsModule::GetModule().StartShippingEngine(...)"));
	}
}

void FsparklogsModule::ShutdownModule()
{
	FCoreDelegates::OnPostEngineInit.RemoveAll(this);
	FCoreDelegates::OnExit.RemoveAll(this);
	if (UObjectInitialized())
	{
		UnregisterSettings();
	}
	// Just in case it was not called earlier...
	StopShippingEngine();
}

bool FsparklogsModule::StartShippingEngine(const TCHAR* OverrideAgentID, const TCHAR* OverrideAgentAuthToken, const TCHAR* OverrideHTTPEndpointURI, const TCHAR* OverrideHttpAuthorizationHeaderValue, const TCHAR* OverrideComputerName, TMap<FString, FString>* AdditionalAttributes, bool AlwaysStart)
{
	if (LoggingActive)
	{
		UE_LOG(LogPluginSparkLogs, Log, TEXT("Logging is already active. Ignoring call to StartShippingEngine."));
		return true;
	}

	FString EffectiveAgentID = Settings->AgentID;
	FString EffectiveAgentAuthToken = Settings->AgentAuthToken;
	FString EffectiveHttpAuthorizationHeaderValue = Settings->HttpAuthorizationHeaderValue;
	if (NULL != OverrideAgentID && FPlatformString::Strlen(OverrideAgentID) > 0)
	{
		EffectiveAgentID = OverrideAgentID;
	}
	if (NULL != OverrideAgentAuthToken && FPlatformString::Strlen(OverrideAgentAuthToken) > 0)
	{
		EffectiveAgentAuthToken = OverrideAgentAuthToken;
	}
	if (NULL != OverrideHttpAuthorizationHeaderValue && FPlatformString::Strlen(OverrideHttpAuthorizationHeaderValue) > 0)
	{
		EffectiveHttpAuthorizationHeaderValue = OverrideHttpAuthorizationHeaderValue;
	}

	bool UsingSparkLogsCloud = !Settings->CloudRegion.IsEmpty();
	FString EffectiveHttpEndpointURI = Settings->GetEffectiveHttpEndpointURI(OverrideHTTPEndpointURI);
	if (EffectiveHttpEndpointURI.IsEmpty())
	{
		UE_LOG(LogPluginSparkLogs, Log, TEXT("Not yet configured for this launch configuration. In plugin settings for %s launch configuration, configure CloudRegion to 'us' or 'eu' for your SparkLogs cloud region (or if you are sending data to your own HTTP service, configure HttpEndpointURI to the appropriate endpoint, such as http://localhost:9880/ or https://ingestlogs.myservice.com/ingest/v1)"), *GetITLINISettingPrefix());
		return false;
	}
	if (UsingSparkLogsCloud && (EffectiveAgentID.IsEmpty() || EffectiveAgentAuthToken.IsEmpty()))
	{
		UE_LOG(LogPluginSparkLogs, Log, TEXT("Not yet configured for this launch configuration. In plugin settings for %s launch configuration, configure authentication credentials to enable. Consider using credentials for Editor vs Client vs Server."), *GetITLINISettingPrefix());
		return false;
	}

	// If we're sending data to the SparkLogs cloud then use lz4 compression by default, otherwise use none as lz4 support is nonstandard.
	if (Settings->CompressionMode == ITLCompressionMode::Default)
	{
		if (UsingSparkLogsCloud || (!EffectiveAgentID.IsEmpty() && !EffectiveAgentAuthToken.IsEmpty()))
		{
			UE_LOG(LogPluginSparkLogs, Log, TEXT("Sending data to SparkLogs cloud, so using lz4 as default compression mode."));
			Settings->CompressionMode = ITLCompressionMode::LZ4;
		}
		else
		{
			UE_LOG(LogPluginSparkLogs, Log, TEXT("Sending data to custom HTTP destination, so using none as default compression mode."));
			Settings->CompressionMode = ITLCompressionMode::None;
		}
	}

	if (!FPlatformProcess::SupportsMultithreading())
	{
		UE_LOG(LogPluginSparkLogs, Warning, TEXT("This plugin cannot run on this platform. This platform does not multithreading."));
		return false;
	}

	float DiceRoll = AlwaysStart ? 10000.0 : FMath::FRandRange(0.0, 100.0);
	LoggingActive = DiceRoll < Settings->ActivationPercentage;
	if (LoggingActive)
	{
		// Log all plugin messages to the ITL operations log
		GLog->AddOutputDevice(GetITLInternalOpsLog().LogDevice.Get());
		// Log all engine messages to an internal log just for this plugin, which we will then read from the file as we push log data to the cloud
		GLog->AddOutputDevice(GetITLInternalGameLog().LogDevice.Get());
	}
	UE_LOG(LogPluginSparkLogs, Log, TEXT("Starting up: LaunchConfiguration=%s, HttpEndpointURI=%s, AgentID=%s, ActivationPercentage=%lf, DiceRoll=%f, Activated=%s"), GetITLLaunchConfiguration(true), *EffectiveHttpEndpointURI, *EffectiveAgentID, Settings->ActivationPercentage, DiceRoll, LoggingActive ? TEXT("yes") : TEXT("no"));
	if (LoggingActive)
	{
		UE_LOG(LogPluginSparkLogs, Log, TEXT("Ingestion parameters: RequestTimeoutSecs=%lf, BytesPerRequest=%d, ProcessingIntervalSecs=%lf, RetryIntervalSecs=%lf"), Settings->RequestTimeoutSecs, Settings->BytesPerRequest, Settings->ProcessingIntervalSecs, Settings->RetryIntervalSecs);
		FString SourceLogFile = GetITLInternalGameLog().LogFilePath;
		FString AuthorizationHeader;
		if (EffectiveHttpAuthorizationHeaderValue.IsEmpty())
		{
			AuthorizationHeader = FString::Format(TEXT("Bearer {0}:{1}"), { *EffectiveAgentID, *EffectiveAgentAuthToken });
		}
		else
		{
			AuthorizationHeader = EffectiveHttpAuthorizationHeaderValue;
		}
		CloudPayloadProcessor = TSharedPtr<FsparklogsWriteHTTPPayloadProcessor>(new FsparklogsWriteHTTPPayloadProcessor(*EffectiveHttpEndpointURI, *AuthorizationHeader, Settings->RequestTimeoutSecs, Settings->DebugLogRequests));
		CloudStreamer = MakeUnique<FsparklogsReadAndStreamToCloud>(*SourceLogFile, Settings, CloudPayloadProcessor.ToSharedRef(), GMaxLineLength, OverrideComputerName, AdditionalAttributes);
		FCoreDelegates::OnExit.AddRaw(this, &FsparklogsModule::OnEngineExit);

		if (Settings->StressTestGenerateIntervalSecs > 0)
		{
			StressGenerator = MakeUnique<FsparklogsStressGenerator>(Settings);
		}
	}
	return LoggingActive;
}

void FsparklogsModule::StopShippingEngine()
{
	if (LoggingActive || CloudStreamer.IsValid())
	{
		UE_LOG(LogPluginSparkLogs, Log, TEXT("Shutting down and flushing logs to cloud..."));
		GLog->Flush();
		if (StressGenerator.IsValid())
		{
			StressGenerator->Stop();
		}
		if (CloudStreamer.IsValid())
		{
			if (CloudPayloadProcessor.IsValid())
			{
				// Set the retry interval to something short so we don't delay shutting down the game...
				Settings->RetryIntervalSecs = 0.2;
				// When the engine is shutting down, wait no more than 6 seconds to flush the final log request
				CloudPayloadProcessor->SetTimeoutSecs(FMath::Min(Settings->RequestTimeoutSecs, 6.0));
			}
			bool LastFlushProcessedEverything = false;
			if (CloudStreamer->FlushAndWait(2, true, true, true, FsparklogsSettings::WaitForFlushToCloudOnShutdown, LastFlushProcessedEverything))
			{
				FString LogFilePath = GetITLInternalGameLog().LogFilePath;
				UE_LOG(LogPluginSparkLogs, Log, TEXT("Flushed logs successfully. LastFlushedEverything=%d"), (int)LastFlushProcessedEverything);
				// Purge this plugin's logfile and delete the progress marker (fully flushed shutdown should start with an empty log next game session).
				FOutputDevice* LogDevice = GetITLInternalGameLog().LogDevice.Get();
				GLog->RemoveOutputDevice(LogDevice);
				LogDevice->Flush();
				LogDevice->TearDown();
				if (LastFlushProcessedEverything)
				{
					UE_LOG(LogPluginSparkLogs, Log, TEXT("All logs fully shipped. Removing progress marker and local logfile %s"), *LogFilePath);
					IFileManager::Get().Delete(*LogFilePath, false, false, false);
					CloudStreamer->DeleteProgressMarker();
				}
			}
			else
			{
				UE_LOG(LogPluginSparkLogs, Log, TEXT("Flush failed or timed out."));
				// NOTE: the progress marker would not have been updated, so we'll keep trying the next time
				// the game engine starts right from where we left off, so we shouldn't lose anything.
			}
			CloudStreamer.Reset();
		}
		CloudPayloadProcessor.Reset();
		StressGenerator.Reset();
		UE_LOG(LogPluginSparkLogs, Log, TEXT("Shutdown."));
		LoggingActive = false;
	}
}

void FsparklogsModule::OnPostEngineInit()
{
	if (UObjectInitialized())
	{
		// Allow the user to edit settings in the project settings editor
		RegisterSettings();
	}
}

void FsparklogsModule::OnEngineExit()
{
	UE_LOG(LogPluginSparkLogs, Log, TEXT("OnEngineExit. Will shutdown the log shipping engine..."));
	StopShippingEngine();
}

void FsparklogsModule::RegisterSettings()
{
	if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
	{
		SettingsModule->RegisterSettings("Project", "Plugins", "SparkLogs",
			LOCTEXT("RuntimeSettingsName", "SparkLogs"),
			LOCTEXT("RuntimeSettingsDescription", "Configure the SparkLogs plugin"),
			GetMutableDefault<USparkLogsRuntimeSettings>());
	}
}

void FsparklogsModule::UnregisterSettings()
{
	if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
	{
		SettingsModule->UnregisterSettings("Project", "Plugins", TEXT("SparkLogs"));
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FsparklogsModule, sparklogs)
