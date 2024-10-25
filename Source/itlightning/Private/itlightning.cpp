// Copyright (C) 2024 IT Lightning, LLC. All rights reserved.
// Licensed software - see LICENSE

#include "itlightning.h"
#include <GenericPlatformOutputDevices.h>
#include <OutputDeviceFile.h>
#include <Interfaces/IHttpResponse.h>
#include <HttpModule.h>

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


#define LOCTEXT_NAMESPACE "FitlightningModule"

DEFINE_LOG_CATEGORY(LogPluginITLightning);

constexpr int GMaxLineLength = 16 * 1024;

static uint8 UTF8ByteOrderMark[3] = {0xEF, 0xBB, 0xBF};

FString ITLConvertUTF8(const void* Data, int Len)
{
	FUTF8ToTCHAR Converter((const ANSICHAR*)(Data), Len);
	return FString(Converter.Length(), Converter.Get());
}

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
	Name.Append(GameMode).Append(TEXT("-")).Append(LogTypeName).Append(TEXT(".log"));
	return Name;
}

FString GetITLPluginStateFilename()
{
	const TCHAR* GameMode = GetITLGameMode(false);
	FString Name = FString(TEXT("itlightning-"), FCString::Strlen(GameMode) + FCString::Strlen(TEXT("-state.ini")));
	Name.Append(GameMode).Append(TEXT("-state.ini"));
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
		Singleton.LogDevice->IncludeCategory(FName(LogPluginITLightning.GetCategoryName().ToString()));
	}
	return Singleton;
}

FitlightningSettings::FitlightningSettings()
	: RequestTimeoutSecs(DefaultRequestTimeoutSecs)
	, ActivationPercent(100.0)
	, BytesPerRequest(DefaultBytesPerRequest)
	, ProcessIntervalSecs(DefaultProcessIntervalSecs)
	, RetryIntervalSecs(DefaultRetryIntervalSecs)
	, IncludeCommonMetadata(DefaultIncludeCommonMetadata)
	, LogRequests(DefaultLogRequests)
{
}

void FitlightningSettings::LoadSettings()
{
	FString Section = GetITLINISectionName();

	HttpEndpointURI = GConfig->GetStr(*Section, TEXT("HttpEndpointURI"), GEngineIni);
	if (!GConfig->GetDouble(*Section, TEXT("RequestTimeoutSecs"), RequestTimeoutSecs, GEngineIni))
	{
		RequestTimeoutSecs = DefaultRequestTimeoutSecs;
	}

	AgentID = GConfig->GetStr(*Section, TEXT("AgentID"), GEngineIni);
	AuthToken = GConfig->GetStr(*Section, TEXT("AuthToken"), GEngineIni);
	
	FString StringActivationPercent;
	GConfig->GetString(*Section, TEXT("ActivationPercent"), StringActivationPercent, GEngineIni);
	StringActivationPercent.TrimStartAndEndInline();
	if (!GConfig->GetDouble(*Section, TEXT("ActivationPercent"), ActivationPercent, GEngineIni))
	{
		ActivationPercent = 100.0;
	}
	else
	{
		// If it was an empty string, treat as 100%
		if (StringActivationPercent.IsEmpty())
		{
			ActivationPercent = 100.0;
		}
	}

	if (!GConfig->GetInt(*Section, TEXT("BytesPerRequest"), BytesPerRequest, GEngineIni))
	{
		BytesPerRequest = DefaultBytesPerRequest;
	}
	if (!GConfig->GetDouble(*Section, TEXT("ProcessIntervalSecs"), ProcessIntervalSecs, GEngineIni))
	{
		ProcessIntervalSecs = DefaultProcessIntervalSecs;
	}
	if (!GConfig->GetDouble(*Section, TEXT("RetryIntervalSecs"), RetryIntervalSecs, GEngineIni))
	{
		RetryIntervalSecs = DefaultRetryIntervalSecs;
	}

	if (!GConfig->GetBool(*Section, TEXT("IncludeCommonMetadata"), IncludeCommonMetadata, GEngineIni))
	{
		IncludeCommonMetadata = DefaultIncludeCommonMetadata;
	}
	if (!GConfig->GetBool(*Section, TEXT("DebugLogRequests"), LogRequests, GEngineIni))
	{
		LogRequests = DefaultLogRequests;
	}

	EnforceConstraints();
}

void FitlightningSettings::EnforceConstraints()
{
	AgentID.TrimStartAndEndInline();
	AuthToken.TrimStartAndEndInline();

	if (RequestTimeoutSecs < MinRequestTimeoutSecs)
	{
		RequestTimeoutSecs = MinRequestTimeoutSecs;
	}
	if (BytesPerRequest < MinBytesPerRequest)
	{
		BytesPerRequest = MinBytesPerRequest;
	}
	if (BytesPerRequest > MaxBytesPerRequest)
	{
		BytesPerRequest = MaxBytesPerRequest;
	}
	if (ProcessIntervalSecs < MinProcessIntervalSecs)
	{
		ProcessIntervalSecs = MinProcessIntervalSecs;
	}
	if (RetryIntervalSecs < MinRetryIntervalSecs)
	{
		RetryIntervalSecs = MinRetryIntervalSecs;
	}
}

FitlightningWriteNDJSONPayloadProcessor::FitlightningWriteNDJSONPayloadProcessor(FString InOutputFilePath) : OutputFilePath(InOutputFilePath) { }

bool FitlightningWriteNDJSONPayloadProcessor::ProcessPayload(const uint8* JSONPayloadInUTF8, int PayloadLen, FitlightningReadAndStreamToCloud* Streamer)
{
	TUniquePtr<IFileHandle> DebugJSONWriter;
	DebugJSONWriter.Reset(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*OutputFilePath, true, true));
	if (DebugJSONWriter == nullptr)
	{
		return false;
	}
	if (!DebugJSONWriter->Write((const uint8*)JSONPayloadInUTF8, PayloadLen)
		|| !DebugJSONWriter->Write((const uint8*)("\r\n"), 2)
		|| !DebugJSONWriter->Flush())
	{
		return false;
	}
	DebugJSONWriter.Reset();
	return true;
}

FitlightningWriteHTTPPayloadProcessor::FitlightningWriteHTTPPayloadProcessor(const TCHAR* InEndpointURI, const TCHAR* InAuthorizationHeader, double InTimeoutSecs, bool InLogRequests)
	: EndpointURI(InEndpointURI)
	, AuthorizationHeader(InAuthorizationHeader)
	, LogRequests(InLogRequests)
{
	SetTimeoutSecs(InTimeoutSecs);
}

void FitlightningWriteHTTPPayloadProcessor::SetTimeoutSecs(double InTimeoutSecs)
{
	TimeoutMillisec.Set((int32)(InTimeoutSecs * 1000.0));
}

bool FitlightningWriteHTTPPayloadProcessor::ProcessPayload(const uint8* JSONPayloadInUTF8, int PayloadLen, FitlightningReadAndStreamToCloud* Streamer)
{
	if (LogRequests)
	{
		int CompressedBufSize = ITLLZ4::LZ4_compressBound(PayloadLen);
		UE_LOG(LogPluginITLightning, Log, TEXT("HTTPPayloadProcessor::ProcessPayload: BEGIN: worst_case_compressed_size=%d, len=%d, timeout_millisec=%d"), CompressedBufSize, PayloadLen, (int)(TimeoutMillisec.GetValue()));
	}
	
	FThreadSafeBool RequestEnded(false);
	FThreadSafeBool RequestSucceeded(false);
	FThreadSafeBool RetryableFailure(true);
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = FHttpModule::Get().CreateRequest();
	HttpRequest->SetURL(*EndpointURI);
	HttpRequest->SetVerb(TEXT("POST"));
	HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json; charset=UTF-8"));
	HttpRequest->SetHeader(TEXT("Authorization"), *AuthorizationHeader);
	HttpRequest->SetTimeout((double)(TimeoutMillisec.GetValue()) / 1000.0);
	InternalBuffer.Reserve(PayloadLen);
	InternalBuffer.Reset(0);
	InternalBuffer.Append(JSONPayloadInUTF8, PayloadLen);
	HttpRequest->SetContent(InternalBuffer);

	HttpRequest->OnProcessRequestComplete().BindLambda([&](FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
		{
			if (LogRequests)
			{
				if (Response.IsValid())
				{
					UE_LOG(LogPluginITLightning, Log, TEXT("HTTPPayloadProcessor::ProcessPayload: RequestComplete: successful=%d, http_status=%d"), bWasSuccessful ? 1 : 0, (int)(Response->GetResponseCode()));
				}
				else
				{
					UE_LOG(LogPluginITLightning, Log, TEXT("HTTPPayloadProcessor::ProcessPayload: RequestComplete: successful=%d, null_response_object"), bWasSuccessful ? 1 : 0);
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
					UE_LOG(LogPluginITLightning, Warning, TEXT("HTTPPayloadProcessor::ProcessPayload: Retryable HTTP response: status=%d, msg=%s"), (int)ResponseCode, *ResponseBody);
					RequestSucceeded.AtomicSet(false);
					RetryableFailure.AtomicSet(true);
				}
				else
				{
					UE_LOG(LogPluginITLightning, Warning, TEXT("HTTPPayloadProcessor::ProcessPayload: Non-Retryable HTTP response: status=%d, msg=%s"), (int)ResponseCode, *ResponseBody);
					RequestSucceeded.AtomicSet(false);
					RetryableFailure.AtomicSet(false);
				}
			}
			else
			{
				UE_LOG(LogPluginITLightning, Warning, TEXT("HTTPPayloadProcessor::ProcessPayload: General HTTP request failure; will retry..."));
				RequestSucceeded.AtomicSet(false);
				RetryableFailure.AtomicSet(true);
			}

			// Signal that the request has finished (success or failure)
			RequestEnded.AtomicSet(true);
		});

	// Start the HTTP request
	double StartTime = FPlatformTime::Seconds();
	if (!HttpRequest->ProcessRequest())
	{
		UE_LOG(LogPluginITLightning, Warning, TEXT("HTTPPayloadProcessor::ProcessPayload: failed to initiate HttpRequest"));
		RequestSucceeded.AtomicSet(false);
		RetryableFailure.AtomicSet(true);
	}
	else
	{
		// Synchronously wait for the request to complete or fail
		while (!RequestEnded)
		{
			// TODO: support cancellation in the future if we need to
			double CurrentTime = FPlatformTime::Seconds();
			double Elapsed = CurrentTime - StartTime;
			// It's possible the timeout has shortened while we've been waiting, so always use the current timeout value
			double Timeout = (double)(TimeoutMillisec.GetValue()) / 1000.0;
			if (Elapsed > Timeout)
			{
				UE_LOG(LogPluginITLightning, Warning, TEXT("HTTPPayloadProcessor::ProcessPayload: Timed out after %.3lf seconds; will retry..."), Elapsed);
				HttpRequest->CancelRequest();
				RequestSucceeded.AtomicSet(false);
				RetryableFailure.AtomicSet(true);
				break;
			}
			FPlatformProcess::SleepNoStats(0.1f);
		}
	}

	// If we had a non-retryable failure, then trigger this worker to stop
	if (!RequestSucceeded && !RetryableFailure)
	{
		if (Streamer != nullptr)
		{
			UE_LOG(LogPluginITLightning, Error, TEXT("HTTPPayloadProcessor::ProcessPayload: stopping log streaming service after non-retryable failure"));
			Streamer->Stop();
		}
	}

	if (LogRequests)
	{
		UE_LOG(LogPluginITLightning, Log, TEXT("HTTPPayloadProcessor::ProcessPayload: END: success=%d, can_retry=%d"), RequestSucceeded ? 1 : 0, RetryableFailure ? 1 : 0);
	}
	return RequestSucceeded;
}

void FitlightningReadAndStreamToCloud::ComputeCommonEventJSON()
{
	FString CommonEventJSON;
	CommonEventJSON.Appendf(TEXT("\"hostname\": %s, \"pid\": %d"), *EscapeJsonString(FPlatformProcess::ComputerName()), FPlatformProcess::GetCurrentProcessId());
	FString ProjectName = FApp::GetProjectName();
	if (ProjectName.Len() > 0 && ProjectName != "None")
	{
		CommonEventJSON.Appendf(TEXT(", \"app\": %s"), *EscapeJsonString(FApp::GetProjectName()));
	}
	int64 CommonEventJSONLen = FTCHARToUTF8_Convert::ConvertedLength(*CommonEventJSON, CommonEventJSON.Len());
	CommonEventJSONData.SetNum(0, false);
	CommonEventJSONData.AddUninitialized(CommonEventJSONLen);
	FTCHARToUTF8_Convert::Convert(CommonEventJSONData.GetData(), CommonEventJSONLen, *CommonEventJSON, CommonEventJSON.Len());
}

FitlightningReadAndStreamToCloud::FitlightningReadAndStreamToCloud(const TCHAR* InSourceLogFile, TSharedRef<FitlightningSettings> InSettings, TSharedRef<IitlightningPayloadProcessor> InPayloadProcessor, int InMaxLineLength)
	: Settings(InSettings)
	, PayloadProcessor(InPayloadProcessor)
	, SourceLogFile(InSourceLogFile)
	, MaxLineLength(InMaxLineLength)
	, Thread(nullptr)
	, WorkerShippedLogOffset(0)
	, WorkerMinNextFlushPlatformTime(0)
{
	ProgressMarkerPath = FPaths::Combine(FPaths::GetPath(InSourceLogFile), GetITLPluginStateFilename());
	if (Settings->IncludeCommonMetadata)
	{
		ComputeCommonEventJSON();
	}

	WorkerBuffer.AddUninitialized(Settings->BytesPerRequest);
	WorkerNextPayload.AddUninitialized(Settings->BytesPerRequest + 4096 + (Settings->BytesPerRequest / 10));
	check(MaxLineLength > 0);
	check(FPlatformProcess::SupportsMultithreading());
	FString ThreadName = FString::Printf(TEXT("ITLightning_Reader_%s"), *FPaths::GetBaseFilename(InSourceLogFile));
	FPlatformAtomics::InterlockedExchangePtr((void**)&Thread, FRunnableThread::Create(this, *ThreadName, 0, TPri_BelowNormal));
}

FitlightningReadAndStreamToCloud::~FitlightningReadAndStreamToCloud()
{
	if (Thread)
	{
		delete Thread;
	}
	Thread = nullptr;
}

bool FitlightningReadAndStreamToCloud::Init()
{
	return true;
}

uint32 FitlightningReadAndStreamToCloud::Run()
{
	WorkerFullyCleanedUp.AtomicSet(false);
	ReadProgressMarker(WorkerShippedLogOffset);
	// A pending flush will be processed before stopping
	while (StopRequestCounter.GetValue() == 0 || FlushRequestCounter.GetValue() > 0)
	{
		// Only allow manual flushes if we are not in a retry delay because the last operation failed.
		if (WorkerLastFlushFailed == false && FlushRequestCounter.GetValue() > 0)
		{
			FlushRequestCounter.Decrement();
			WorkerDoFlush();
		}
		else if (FPlatformTime::Seconds() > WorkerMinNextFlushPlatformTime)
		{
			// If we are waiting on a manual flush, and the retry timer finally expired, it's OK to mark this attempt as processing it.
			if (FlushRequestCounter.GetValue() > 0)
			{
				FlushRequestCounter.Decrement();
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
	return 0;
}

void FitlightningReadAndStreamToCloud::Stop()
{
	StopRequestCounter.Increment();
}

bool FitlightningReadAndStreamToCloud::FlushAndWait(int N, bool ClearRetryTimer, bool InitiateStop, double TimeoutSec, bool& OutLastFlushProcessedEverything)
{
	OutLastFlushProcessedEverything = false;
	bool WasSuccessful = true;

	// If we've already requested a stop, a flush is impossible
	if (StopRequestCounter.GetValue() > 0)
	{
		return false;
	}

	if (ClearRetryTimer)
	{
		WorkerLastFlushFailed.AtomicSet(false);
	}

	for (int i = 0; i < N; i++)
	{
		int StartFlushSuccessOpCounter = FlushSuccessOpCounter.GetValue();
		int StartFlushOpCounter = FlushOpCounter.GetValue();
		FlushRequestCounter.Increment();
		// Last time around, we might initiate a stop
		if (InitiateStop && i == N-1)
		{
			Stop();
		}
		double StartTime = FPlatformTime::Seconds();
		while (FlushOpCounter.GetValue() == StartFlushOpCounter)
		{
			if (FPlatformTime::Seconds() - StartTime > TimeoutSec)
			{
				return false;
			}
			FPlatformProcess::SleepNoStats(0.05f);
		}
		WasSuccessful = FlushSuccessOpCounter.GetValue() != StartFlushSuccessOpCounter;
	}
	if (WasSuccessful)
	{
		OutLastFlushProcessedEverything = LastFlushProcessedEverything;
	}
	if (InitiateStop) {
		// Wait for the worker to fully stop, up to the timeout
		double StartTime = FPlatformTime::Seconds();
		while (!WorkerFullyCleanedUp)
		{
			if (FPlatformTime::Seconds() - StartTime > TimeoutSec)
			{
				return false;
			}
			FPlatformProcess::SleepNoStats(0.01f);
		}
	}
	return WasSuccessful;
}

bool FitlightningReadAndStreamToCloud::ReadProgressMarker(int64& OutMarker)
{
	OutMarker = 0;
	double OutDouble = 0.0;
	if (IFileManager::Get().FileExists(*ProgressMarkerPath))
	{
		bool WasDisabled = GConfig->AreFileOperationsDisabled();
		GConfig->EnableFileOperations();
		bool Result = GConfig->GetDouble(FitlightningSettings::PluginStateSection, ProgressMarkerValue, OutDouble, *ProgressMarkerPath);
		if (WasDisabled)
		{
			GConfig->DisableFileOperations();
		}
		if (!Result)
		{
			UE_LOG(LogPluginITLightning, Warning, TEXT("Failed to read progress marker from %s"), *ProgressMarkerPath);
			return false;
		}
	}
	// Precise to 52+ bits
	OutMarker = (int64)(OutDouble);
	return true;
}

bool FitlightningReadAndStreamToCloud::WriteProgressMarker(int64 InMarker)
{
	// TODO: should we use the sqlite plugin instead, maybe it's not as much overhead as writing INI file each time?
	// Precise to 52+ bits
	bool WasDisabled = GConfig->AreFileOperationsDisabled();
	GConfig->EnableFileOperations();
	GConfig->SetDouble(FitlightningSettings::PluginStateSection, ProgressMarkerValue, (double)(InMarker), *ProgressMarkerPath);
	GConfig->Flush(false, ProgressMarkerPath);
	if (WasDisabled)
	{
		GConfig->DisableFileOperations();
	}
	return true;
}

void FitlightningReadAndStreamToCloud::DeleteProgressMarker()
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

bool FitlightningReadAndStreamToCloud::WorkerBuildNextPayload(int NumToRead, int& OutCapturedOffset, int& OutNumCapturedLines)
{
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
			ITL_DBG_UE_LOG(LogPluginITLightning, Display, TEXT("STREAMER: skipping UTF8 BOM: offset_before=%d, offset_after=%d"), NextOffset, NextOffset + sizeof(UTF8ByteOrderMark));
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
		ITL_DBG_UE_LOG(LogPluginITLightning, Display, TEXT("STREAMER: after newline search: NextOffset=%d, HaveLine=%d, NumToSearch=%d, FoundIndex=%d"), NextOffset, (int)HaveLine, NumToSearch, FoundIndex);
		if (!HaveLine && NumToSearch == MaxLineLength && RemainingBytes > NumToSearch)
		{
			// Even though we didn't find a line, break the line at the max length and process it
			// It's unsafe to break a line in the middle of a multi-byte UTF-8, so find a safe break point...
			ExtraToSkip = 0;
			FoundIndex = MaxLineLength - 1;
			ITL_DBG_UE_LOG(LogPluginITLightning, Display, TEXT("STREAMER: no newline found, search for safe breakpoint: NextOffset=%d, FoundIndex=%d"), NextOffset, FoundIndex);
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
			ITL_DBG_UE_LOG(LogPluginITLightning, Display, TEXT("STREAMER: found safe breakpoint: NextOffset=%d, FoundIndex=%d, ExtraToSkip=%d"), NextOffset, FoundIndex, ExtraToSkip);
		}
		if (!HaveLine)
		{
			// No more complete lines to process, this is enough for now
			ITL_DBG_UE_LOG(LogPluginITLightning, Display, TEXT("STREAMER: no more lines to process, break"));
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
				ITL_DBG_UE_LOG(LogPluginITLightning, Display, TEXT("STREAMER: character at NextOffset=%d, FoundIndex=%d is newline, will skip it"), NextOffset, FoundIndex);
				ExtraToSkip++;
				FoundIndex--;
			}
			else
			{
				break;
			}
		}
		ITL_DBG_UE_LOG(LogPluginITLightning, Display, TEXT("STREAMER: line summary: NextOffset=%d, FoundIndex=%d, ExtraToSkip=%d"), NextOffset, FoundIndex, ExtraToSkip);
		// Skip blank lines without capturing anything
		if (FoundIndex <= 0)
		{
			ITL_DBG_UE_LOG(LogPluginITLightning, Display, TEXT("STREAMER: skipping blank line..."));
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
		ITL_DBG_UE_LOG(LogPluginITLightning, Display, TEXT("STREAMER: adding message to payload: %s"), *ITLConvertUTF8(BufferData + NextOffset, FoundIndex));
		WorkerNextPayload.Append('}');
		OutNumCapturedLines++;
		NextOffset += FoundIndex + ExtraToSkip;
		OutCapturedOffset = NextOffset;
	}
	WorkerNextPayload.Append(']');
	return true;
}

bool FitlightningReadAndStreamToCloud::WorkerInternalDoFlush(int64& OutNewShippedLogOffset, bool& OutFlushProcessedEverything)
{
	int64 EffectiveShippedLogOffset = WorkerShippedLogOffset;
	OutNewShippedLogOffset = EffectiveShippedLogOffset;
	OutFlushProcessedEverything = false;
	
	// Re-open the file. UE doesn't contain cross-platform class that can stay open and refresh the filesize OR to read up to N (but maybe less than N bytes).
	// The only solution and stay within UE class library is to just re-open the file every flush request. This is actually quite fast on modern platforms.
	TUniquePtr<IFileHandle> WorkerReader;
	WorkerReader.Reset(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*SourceLogFile, true));
	if (WorkerReader == nullptr)
	{
		UE_LOG(LogPluginITLightning, Warning, TEXT("STREAMER: Failed to open logfile='%s'"), *SourceLogFile);
		return false;
	}
	int64 FileSize = WorkerReader->Size();
	ITL_DBG_UE_LOG(LogPluginITLightning, Display, TEXT("STREAMER: opened log file: last_offset=%ld, current_file_size=%ld, logfile='%s'"), EffectiveShippedLogOffset, FileSize, *SourceLogFile);
	if (EffectiveShippedLogOffset > FileSize)
	{
		UE_LOG(LogPluginITLightning, Log, TEXT("STREAMER: Logfile reduced size, re-reading from start: new_size=%ld, previously_processed_to=%ld, logfile='%s'"), FileSize, EffectiveShippedLogOffset, *SourceLogFile);
		EffectiveShippedLogOffset = 0;
	}
	// Start at the last known shipped position, read as many bytes as possible up to the max buffer size, and capture log lines into a JSON payload
	WorkerReader->Seek(EffectiveShippedLogOffset);
	int64 RemainingBytes = FileSize - EffectiveShippedLogOffset;
	int NumToRead = (int)(FMath::Clamp<int64>(RemainingBytes, 0, (int64)(WorkerBuffer.Num())));
	if (NumToRead <= 0)
	{
		// We've read everything we possibly can already
		OutFlushProcessedEverything = true;
		return true;
	}

	uint8* BufferData = WorkerBuffer.GetData();
	if (!WorkerReader->Read(BufferData, NumToRead))
	{
		UE_LOG(LogPluginITLightning, Warning, TEXT("STREAMER: Failed to read data: offset=%ld, bytes=%ld, logfile='%s'"), EffectiveShippedLogOffset, NumToRead, *SourceLogFile);
		return false;
	}
	ITL_DBG_UE_LOG(LogPluginITLightning, Display, TEXT("STREAMER: read data into buffer: offset=%ld, data_len=%d, data=%s, logfile='%s'"), EffectiveShippedLogOffset, NumToRead, *ITLConvertUTF8(BufferData, NumToRead), *SourceLogFile);
	
	int CapturedOffset = 0;
	int NumCapturedLines = 0;
	if (!WorkerBuildNextPayload(NumToRead, CapturedOffset, NumCapturedLines))
	{
		UE_LOG(LogPluginITLightning, Warning, TEXT("STREAMER: Failed to build payload: offset=%ld, captured_offset=%d, logfile='%s'"), EffectiveShippedLogOffset, CapturedOffset, *SourceLogFile);
		return false;
	}

	ITL_DBG_UE_LOG(LogPluginITLightning, Display, TEXT("STREAMER: payload is ready to process: offset=%ld, captured_offset=%d, captured_lines=%d, data_len=%d, data=%s, logfile='%s'"),
		EffectiveShippedLogOffset, CapturedOffset, NumCapturedLines, WorkerNextPayload.Len(), *ITLConvertUTF8(WorkerNextPayload.GetData(), WorkerNextPayload.Len()), *SourceLogFile);
	if (NumCapturedLines > 0)
	{
		if (!PayloadProcessor->ProcessPayload((const uint8*)WorkerNextPayload.GetData(), WorkerNextPayload.Len(), this))
		{
			UE_LOG(LogPluginITLightning, Display, TEXT("STREAMER: Failed to process payload: offset=%ld, captured_offset=%d, logfile='%s'"), EffectiveShippedLogOffset, CapturedOffset, *SourceLogFile);
			return false;
		}
	}
	int ProcessedOffset = CapturedOffset;

	// If we processed everything up until the end of the file, we captured everything we can.
	OutNewShippedLogOffset = EffectiveShippedLogOffset + ProcessedOffset;
	if ((int64)(ProcessedOffset) >= RemainingBytes)
	{
		OutFlushProcessedEverything = true;
	}
	return true;
}

bool FitlightningReadAndStreamToCloud::WorkerDoFlush()
{
	int64 ShippedNewLogOffset = 0;
	bool FlushProcessedEverything = false;
	bool Result = WorkerInternalDoFlush(ShippedNewLogOffset, FlushProcessedEverything);
	if (!Result)
	{
		WorkerLastFlushFailed.AtomicSet(true);
		WorkerMinNextFlushPlatformTime = FPlatformTime::Seconds() + Settings->RetryIntervalSecs;
		LastFlushProcessedEverything.AtomicSet(false);
	}
	else
	{
		WorkerLastFlushFailed.AtomicSet(false);
		WorkerShippedLogOffset = ShippedNewLogOffset;
		WriteProgressMarker(ShippedNewLogOffset);
		WorkerMinNextFlushPlatformTime = FPlatformTime::Seconds() + Settings->ProcessIntervalSecs;
		LastFlushProcessedEverything.AtomicSet(FlushProcessedEverything);
		FlushSuccessOpCounter.Increment();
	}
	FlushOpCounter.Increment();
	return Result;
}

FitlightningModule::FitlightningModule()
	: LoggingActive(false)
	, Settings(new FitlightningSettings())
{
}

void FitlightningModule::StartupModule()
{
	// TODO: does it matter if we are loaded later and miss a bunch of log entries during engine initialization?
	// TODO: Should run plugin earlier and check command line to determine if this is running in an editor with
	//       similar logic to FEngineLoop::PreInitPreStartupScreen [LaunchEngineLoop.cpp] (GIsEditor not available earlier).
	//       If we change it here, also change GetITLGameMode.
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

	Settings->LoadSettings();
	if (Settings->AgentID.IsEmpty() || Settings->AuthToken.IsEmpty())
	{
		UE_LOG(LogPluginITLightning, Log, TEXT("Not yet configured for this game mode. In DefaultEngine.ini section %s configure AgentID and AuthToken to enable. Consider using a different agent for Editor vs Client vs Server mode."), *GetITLINISectionName());
		return;
	}
	if (Settings->HttpEndpointURI.IsEmpty())
	{
		UE_LOG(LogPluginITLightning, Log, TEXT("Not yet configured for this game mode. In DefaultEngine.ini section %s configure HttpEndpointURI to the appropriate endpoint, such as https://ingest-<REGION>.engine.itlightning.app/ingest/v1"), *GetITLINISectionName());
		return;
	}

	if (!FPlatformProcess::SupportsMultithreading())
	{
		UE_LOG(LogPluginITLightning, Warning, TEXT("This plugin cannot run on this platform. This platform does not multithreading."));
		return;
	}

	float DiceRoll = FMath::FRandRange(0.0, 100.0);
	LoggingActive = DiceRoll < Settings->ActivationPercent;
	if (LoggingActive)
	{
		// Log all IT Lightning messages to the ITL operations log
		GLog->AddOutputDevice(GetITLInternalOpsLog().LogDevice.Get());
		// Log all engine messages to an internal log just for this plugin, which we will then read from the file as we push log data to the cloud
		GLog->AddOutputDevice(GetITLInternalGameLog().LogDevice.Get());
	}
	UE_LOG(LogPluginITLightning, Log, TEXT("Starting up: GameMode=%s, HttpEndpointURI=%s, AgentID=%s, ActivationPercent=%lf, DiceRoll=%f, Activated=%s"), GetITLGameMode(true), *Settings->HttpEndpointURI, *Settings->AgentID, Settings->ActivationPercent, DiceRoll, LoggingActive ? TEXT("yes") : TEXT("no"));
	if (LoggingActive)
	{
		UE_LOG(LogPluginITLightning, Log, TEXT("Ingestion parameters: RequestTimeoutSecs=%lf, BytesPerRequest=%d, ProcessIntervalSecs=%lf, RetryIntervalSecs=%lf"), Settings->RequestTimeoutSecs, Settings->BytesPerRequest, Settings->ProcessIntervalSecs, Settings->RetryIntervalSecs);
		FString SourceLogFile = GetITLInternalGameLog().LogFilePath;
		FString AuthorizationHeader = FString::Format(TEXT("Bearer {0}:{1}"), { *Settings->AgentID, *Settings->AuthToken });
		CloudPayloadProcessor = TSharedPtr<FitlightningWriteHTTPPayloadProcessor>(new FitlightningWriteHTTPPayloadProcessor(*Settings->HttpEndpointURI, *AuthorizationHeader, Settings->RequestTimeoutSecs, Settings->LogRequests));
		CloudStreamer = MakeUnique<FitlightningReadAndStreamToCloud>(*SourceLogFile, Settings, CloudPayloadProcessor.ToSharedRef(), GMaxLineLength);
	}
}

void FitlightningModule::ShutdownModule()
{
	if (LoggingActive || CloudStreamer.IsValid())
	{
		UE_LOG(LogPluginITLightning, Log, TEXT("Shutting down and flushing logs to cloud..."));
		GLog->Flush();
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
			if (CloudStreamer->FlushAndWait(2, true, true, FitlightningSettings::WaitForFlushToCloudOnShutdown, LastFlushProcessedEverything))
			{
				FString LogFilePath = GetITLInternalGameLog().LogFilePath;
				UE_LOG(LogPluginITLightning, Log, TEXT("Flushed logs successfully. LastFlushedEverything=%d"), (int)LastFlushProcessedEverything);
				// Purge the IT Lightning logfile and delete the progress marker (fully flushed shutdown should start with an empty log next game session).
				FOutputDevice* LogDevice = GetITLInternalGameLog().LogDevice.Get();
				GLog->RemoveOutputDevice(LogDevice);
				LogDevice->Flush();
				LogDevice->TearDown();
				if (LastFlushProcessedEverything)
				{
					UE_LOG(LogPluginITLightning, Log, TEXT("All logs fully shipped. Removing progress marker and local logfile %s"), *LogFilePath);
					IFileManager::Get().Delete(*LogFilePath, false, false, false);
					CloudStreamer->DeleteProgressMarker();
				}
			}
			else
			{
				UE_LOG(LogPluginITLightning, Log, TEXT("Flush failed or timed out."));
				// NOTE: the progress marker would not have been updated, so we'll keep trying the next time
				// the game engine starts right from where we left off, so we shouldn't lose anything.
			}
			CloudStreamer.Reset();
		}
		CloudPayloadProcessor.Reset();
		UE_LOG(LogPluginITLightning, Log, TEXT("Shutdown."));
		LoggingActive = false;
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FitlightningModule, itlightning)
