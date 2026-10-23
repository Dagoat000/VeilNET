#include "EasyHWIDBlueprintLibrary.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#include <bcrypt.h>

namespace
{
	constexpr int32 RawSmbiosHeaderSize = 8;
	constexpr DWORD SmbiosProvider =
		'R' | ('S' << 8) | ('M' << 16) | ('B' << 24);

	FString GetSmbiosString(
		const TArray<uint8>& Data,
		int32 Start,
		int32 End,
		uint8 StringIndex)
	{
		if (StringIndex == 0)
		{
			return FString();
		}

		uint8 CurrentIndex = 0;
		while (Start < End)
		{
			if (Data[Start] == 0
				&& Start + 1 < End
				&& Data[Start + 1] == 0)
			{
				break;
			}

			const int32 StringStart = Start;
			while (Start < End && Data[Start] != 0)
			{
				++Start;
			}

			if (Start >= End)
			{
				break;
			}

			++CurrentIndex;
			if (CurrentIndex == StringIndex)
			{
				const FUTF8ToTCHAR Converted(
					reinterpret_cast<const ANSICHAR*>(
						Data.GetData() + StringStart),
					Start - StringStart);
				return FString(Converted.Length(), Converted.Get());
			}
			++Start;
		}

		return FString();
	}

	FString NormalizeSerial(FString Serial)
	{
		Serial.TrimStartAndEndInline();
		Serial.ToUpperInline();

		FString CompactSerial = Serial;
		CompactSerial.ReplaceInline(TEXT(" "), TEXT(""));
		CompactSerial.ReplaceInline(TEXT("."), TEXT(""));
		CompactSerial.ReplaceInline(TEXT("_"), TEXT(""));
		if (CompactSerial.IsEmpty()
			|| CompactSerial == TEXT("TOBEFILLEDBYOEM")
			|| CompactSerial == TEXT("DEFAULTSTRING")
			|| CompactSerial == TEXT("SYSTEMSERIALNUMBER")
			|| CompactSerial == TEXT("UNKNOWN")
			|| CompactSerial == TEXT("NONE")
			|| CompactSerial == TEXT("NOTSPECIFIED"))
		{
			return FString();
		}

		return Serial;
	}

	bool GetHardwareIdentifiers(FString& OutHardwareIdentifiers)
	{
		const DWORD RequiredSize = GetSystemFirmwareTable(
			SmbiosProvider,
			0,
			nullptr,
			0);
		if (RequiredSize <= RawSmbiosHeaderSize)
		{
			return false;
		}

		TArray<uint8> RawData;
		RawData.SetNumUninitialized(static_cast<int32>(RequiredSize));
		const DWORD BytesWritten = GetSystemFirmwareTable(
			SmbiosProvider,
			0,
			RawData.GetData(),
			RequiredSize);
		if (BytesWritten <= RawSmbiosHeaderSize)
		{
			return false;
		}

		uint32 TableLength = 0;
		FMemory::Memcpy(
			&TableLength,
			RawData.GetData() + 4,
			sizeof(TableLength));
		const uint64 TableEnd64 =
			static_cast<uint64>(RawSmbiosHeaderSize) + TableLength;
		if (TableEnd64 > BytesWritten)
		{
			return false;
		}

		const int32 TableEnd = static_cast<int32>(TableEnd64);
		FString SystemUuid;
		FString BoardSerial;

		for (int32 Offset = RawSmbiosHeaderSize;
			Offset + 4 <= TableEnd;)
		{
			const uint8 Type = RawData[Offset];
			const uint8 StructureLength = RawData[Offset + 1];
			if (StructureLength < 4
				|| Offset + StructureLength > TableEnd)
			{
				return false;
			}

			const int32 StringsStart = Offset + StructureLength;
			if (Type == 1 && StructureLength >= 0x19)
			{
				const uint8* Uuid = RawData.GetData() + Offset + 8;
				bool bAllZero = true;
				bool bAllOnes = true;
				for (int32 Index = 0; Index < 16; ++Index)
				{
					bAllZero &= Uuid[Index] == 0;
					bAllOnes &= Uuid[Index] == 0xFF;
				}

				if (!bAllZero && !bAllOnes)
				{
					SystemUuid = BytesToHex(Uuid, 16).ToLower();
				}
			}
			else if (Type == 2 && StructureLength >= 8)
			{
				BoardSerial = NormalizeSerial(GetSmbiosString(
					RawData,
					StringsStart,
					TableEnd,
					RawData[Offset + 7]));
			}

			int32 NextOffset = StringsStart;
			while (NextOffset + 1 < TableEnd
				&& !(RawData[NextOffset] == 0
					&& RawData[NextOffset + 1] == 0))
			{
				++NextOffset;
			}
			if (NextOffset + 1 >= TableEnd)
			{
				return false;
			}

			Offset = NextOffset + 2;
			if (Type == 127)
			{
				break;
			}
		}

		if (SystemUuid.IsEmpty() && BoardSerial.IsEmpty())
		{
			return false;
		}

		OutHardwareIdentifiers = TEXT("smbios.uuid=") + SystemUuid
			+ TEXT("\nsmbios.board=") + BoardSerial;
		return true;
	}

	bool HashHardwareIdentifiers(
		const FString& HardwareIdentifiers,
		FString& OutFingerprint)
	{
		FTCHARToUTF8 EncodedIdentifiers(*HardwareIdentifiers);
		BCRYPT_ALG_HANDLE Algorithm = nullptr;
		BCRYPT_HASH_HANDLE Hash = nullptr;
		uint8 Digest[32] = {};

		NTSTATUS Status = BCryptOpenAlgorithmProvider(
			&Algorithm,
			BCRYPT_SHA256_ALGORITHM,
			nullptr,
			0);
		if (Status >= 0)
		{
			Status = BCryptCreateHash(
				Algorithm,
				&Hash,
				nullptr,
				0,
				nullptr,
				0,
				0);
		}
		if (Status >= 0)
		{
			Status = BCryptHashData(
				Hash,
				reinterpret_cast<PUCHAR>(
					const_cast<ANSICHAR*>(EncodedIdentifiers.Get())),
				static_cast<ULONG>(EncodedIdentifiers.Length()),
				0);
		}
		if (Status >= 0)
		{
			Status = BCryptFinishHash(Hash, Digest, sizeof(Digest), 0);
		}

		if (Hash)
		{
			BCryptDestroyHash(Hash);
		}
		if (Algorithm)
		{
			BCryptCloseAlgorithmProvider(Algorithm, 0);
		}

		if (Status < 0)
		{
			return false;
		}

		OutFingerprint = BytesToHex(Digest, UE_ARRAY_COUNT(Digest)).ToLower();
		return true;
	}
}
#endif

bool UEasyHWIDBlueprintLibrary::GetHardwareFingerprint(FString& OutFingerprint)
{
	OutFingerprint.Reset();

#if PLATFORM_WINDOWS
	FString HardwareIdentifiers;
	if (!GetHardwareIdentifiers(HardwareIdentifiers))
	{
		return false;
	}

	return HashHardwareIdentifiers(HardwareIdentifiers, OutFingerprint);
#else
	return false;
#endif
}
