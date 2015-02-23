#include "stdafx.h"
#include "NinSnesSeq.h"
#include "NinSnesFormat.h"
#include "SeqEvent.h"
#include "ScaleConversion.h"

DECLARE_FORMAT(NinSnes);

//  **********
//  NinSnesSeq
//  **********
#define MAX_TRACKS  8
#define SEQ_PPQN    48
#define SEQ_KEYOFS  24

#define NINSNES_INTELLI_FE3FLAGS_NEW_NOTE_PARAM 0x80

NinSnesSeq::NinSnesSeq(RawFile* file, NinSnesVersion ver, uint32_t offset, uint8_t percussion_base, const std::vector<uint8_t>& theVolumeTable, const std::vector<uint8_t>& theDurRateTable, std::wstring theName)
	: VGMMultiSectionSeq(NinSnesFormat::name, file, offset, 0, theName), version(ver),
	header(NULL),
	volumeTable(theVolumeTable),
	durRateTable(theDurRateTable),
	spcPercussionBaseInit(percussion_base),
	konamiBaseAddress(0)
{
	bLoadTickByTick = true;
	bAllowDiscontinuousTrackData = true;

	UseReverb();
	AlwaysWriteInitialReverb(0);

	LoadEventMap();
}

NinSnesSeq::~NinSnesSeq()
{
}

void NinSnesSeq::ResetVars()
{
	VGMMultiSectionSeq::ResetVars();

	spcPercussionBase = spcPercussionBaseInit;
	sectionRepeatCount = 0;

	for (int trackIndex = 0; trackIndex < MAX_TRACKS; trackIndex++) {
		sharedTrackData[trackIndex].ResetVars();
	}
}

bool NinSnesSeq::GetHeaderInfo()
{
	SetPPQN(SEQ_PPQN);
	nNumTracks = MAX_TRACKS;

	if (dwStartOffset + 2 > 0x10000) {
		return false;
	}

	// validate first section
	uint16_t firstSectionPtr = dwStartOffset;
	uint16_t addrFirstSection = GetShort(firstSectionPtr);
	if (addrFirstSection + 16 > 0x10000) {
		return false;
	}

	if (addrFirstSection >= 0x0100) {
		addrFirstSection = ConvertToAPUAddress(addrFirstSection);

		uint8_t numActiveTracks = 0;
		for (uint8_t trackIndex = 0; trackIndex < MAX_TRACKS; trackIndex++) {
			uint16_t addrTrackStart = GetShort(addrFirstSection + trackIndex * 2);
			if (addrTrackStart != 0) {
				addrTrackStart = ConvertToAPUAddress(addrTrackStart);

				if (addrTrackStart < addrFirstSection) {
					return false;
				}
				numActiveTracks++;
			}
		}
		if (numActiveTracks == 0) {
			return false;
		}
	}

	// events will be added later, see ReadEvent
	header = AddHeader(dwStartOffset, 0);
	return true;
}

bool NinSnesSeq::ReadEvent(long stopTime)
{
	uint32_t beginOffset = curOffset;
	if (curOffset + 1 >= 0x10000) {
		return false;
	}

	if (curOffset < header->dwOffset) {
		uint32_t distance = header->dwOffset - curOffset;
		header->dwOffset = curOffset;
		if (header->unLength != 0) {
			header->unLength += distance;
		}
	}

	uint16_t sectionAddress = GetShort(curOffset); curOffset += 2;
	bool bContinue = true;

	if (sectionAddress == 0) {
		// End
		if (!IsOffsetUsed(beginOffset)) {
			header->AddSimpleItem(beginOffset, curOffset - beginOffset, L"Section Playlist End");
		}
		bContinue = false;
	}
	else if (sectionAddress <= 0xff) {
		// Jump
		if (curOffset + 1 >= 0x10000) {
			return false;
		}

		uint16_t repeatCount = sectionAddress;
		uint16_t dest = GetShortAddress(curOffset); curOffset += 2;

		bool startNewRepeat = false;
		bool doJump = false;
		bool infiniteLoop = false;

		if (version == NINSNES_TOSE) {
			if (sectionRepeatCount == 0) {
				// set new repeat count
				sectionRepeatCount = (uint8_t)repeatCount;
				startNewRepeat = true;
				doJump = true;
			}
			else {
				// infinite loop?
				if (sectionRepeatCount == 0xff) {
					if (IsOffsetUsed(dest)) {
						infiniteLoop = true;
					}
					doJump = true;
				}
				else {
					// decrease 8-bit counter
					sectionRepeatCount--;

					// jump if the counter is zero
					if (sectionRepeatCount != 0) {
						doJump = true;
					}
				}
			}
		}
		else {
			// decrease 8-bit counter
			sectionRepeatCount--;
			// check overflow (end of loop)
			if (sectionRepeatCount >= 0x80) {
				// set new repeat count
				sectionRepeatCount = (uint8_t)repeatCount;
				startNewRepeat = true;
			}

			// jump if the counter is zero
			if (sectionRepeatCount != 0) {
				doJump = true;
				if (IsOffsetUsed(dest) && sectionRepeatCount > 0x80) {
					infiniteLoop = true;
				}
			}
		}

		// add event to sequence
		if (!IsOffsetUsed(beginOffset)) {
			header->AddSimpleItem(beginOffset, curOffset - beginOffset, L"Playlist Jump");

			// add the last event too, if available
			if (curOffset + 1 < 0x10000 && GetShort(curOffset) == 0x0000) {
				header->AddSimpleItem(curOffset, 2, L"Playlist End");
			}
		}

		if (infiniteLoop) {
			bContinue = AddLoopForeverNoItem();
		}

		// do actual jump, at last
		if (doJump) {
			curOffset = dest;

			if (curOffset < dwOffset) {
				uint32_t distance = dwOffset - curOffset;
				dwOffset = curOffset;
				if (unLength != 0) {
					unLength += distance;
				}
			}
		}
	}
	else {
		sectionAddress = ConvertToAPUAddress(sectionAddress);

		// Play the section
		if (!IsOffsetUsed(beginOffset)) {
			header->AddSimpleItem(beginOffset, curOffset - beginOffset, L"Section Pointer");
		}

		NinSnesSection* section = (NinSnesSection*)GetSectionFromOffset(sectionAddress);
		if (section == NULL) {
			section = new NinSnesSection(this, sectionAddress);
			if (!section->Load()) {
				pRoot->AddLogItem(new LogItem(L"Failed to load section\n", LOG_LEVEL_ERR, L"NinSnesSeq"));
				return false;
			}
			AddSection(section);
		}

		if (!LoadSection(section, stopTime)) {
			bContinue = false;
		}

		if (version == NINSNES_UNKNOWN) {
			bContinue = false;
		}
	}

	return bContinue;
}

void NinSnesSeq::LoadEventMap()
{
	int statusByte;

	if (version == NINSNES_UNKNOWN) {
		return;
	}

	const uint8_t NINSNES_VOL_TABLE_EARLIER[16] = {
		0x08, 0x12, 0x1b, 0x24, 0x2c, 0x35, 0x3e, 0x47,
		0x51, 0x5a, 0x62, 0x6b, 0x7d, 0x8f, 0xa1, 0xb3,
	};

	const uint8_t NINSNES_DUR_TABLE_EARLIER[8] = {
		0x33, 0x66, 0x80, 0x99, 0xb3, 0xcc, 0xe6, 0xff,
	};

	const uint8_t NINSNES_PAN_TABLE_EARLIER[21] = {
		0x00, 0x01, 0x03, 0x07, 0x0d, 0x15, 0x1e, 0x29,
		0x34, 0x42, 0x51, 0x5e, 0x67, 0x6e, 0x73, 0x77,
		0x7a, 0x7c, 0x7d, 0x7e, 0x7f,
	};

	const uint8_t NINSNES_VOL_TABLE_STANDARD[16] = {
		0x19, 0x33, 0x4c, 0x66, 0x72, 0x7f, 0x8c, 0x99,
		0xa5, 0xb2, 0xbf, 0xcc, 0xd8, 0xe5, 0xf2, 0xfc,
	};

	const uint8_t NINSNES_DUR_TABLE_STANDARD[8] = {
		0x33, 0x66, 0x7f, 0x99, 0xb2, 0xcc, 0xe5, 0xfc,
	};

	const uint8_t NINSNES_PAN_TABLE_STANDARD[21] = {
		0x00, 0x01, 0x03, 0x07, 0x0d, 0x15, 0x1e, 0x29,
		0x34, 0x42, 0x51, 0x5e, 0x67, 0x6e, 0x73, 0x77,
		0x7a, 0x7c, 0x7d, 0x7e, 0x7f,
	};

	const uint8_t NINSNES_DURVOL_TABLE_INTELLI_FE3[64] = {
		0x00, 0x0c, 0x19, 0x26, 0x33, 0x3f, 0x4c, 0x59,
		0x66, 0x72, 0x75, 0x77, 0x70, 0x7c, 0x7f, 0x82,
		0x84, 0x87, 0x89, 0x8c, 0x8e, 0x91, 0x93, 0x96,
		0x99, 0x9b, 0x9e, 0xa0, 0xa3, 0xa5, 0xa8, 0xaa,
		0xad, 0xaf, 0xb2, 0xb5, 0xb7, 0xba, 0xbc, 0xbf,
		0xc1, 0xc4, 0xc6, 0xc9, 0xcc, 0xce, 0xd1, 0xd3,
		0xd6, 0xd8, 0xdb, 0xdd, 0xe0, 0xe2, 0xe5, 0xe8,
		0xea, 0xed, 0xef, 0xf2, 0xf4, 0xf7, 0xf9, 0xfc,
	};

	const uint8_t NINSNES_DURVOL_TABLE_INTELLI_FE4[64] = {
		0x19, 0x26, 0x33, 0x3f, 0x4c, 0x59, 0x66, 0x6d,
		0x70, 0x72, 0x75, 0x77, 0x70, 0x7c, 0x7f, 0x82,
		0x84, 0x87, 0x89, 0x8c, 0x8e, 0x91, 0x93, 0x96,
		0x99, 0x9b, 0x9e, 0xa0, 0xa3, 0xa5, 0xa8, 0xaa,
		0xad, 0xaf, 0xb2, 0xb5, 0xb7, 0xba, 0xbc, 0xbf,
		0xc1, 0xc4, 0xc6, 0xc9, 0xcc, 0xce, 0xd1, 0xd3,
		0xd6, 0xd8, 0xdb, 0xdd, 0xe0, 0xe2, 0xe5, 0xe8,
		0xea, 0xed, 0xef, 0xf2, 0xf4, 0xf7, 0xf9, 0xfc,
	};

	switch (version) {
	case NINSNES_EARLIER:
		STATUS_END = 0x00;
		STATUS_NOTE_MIN = 0x80;
		STATUS_NOTE_MAX = 0xc5;
		STATUS_PERCUSSION_NOTE_MIN = 0xd0;
		STATUS_PERCUSSION_NOTE_MAX = 0xd9;
		break;

	default:
		STATUS_END = 0x00;
		STATUS_NOTE_MIN = 0x80;
		STATUS_NOTE_MAX = 0xc7;
		STATUS_PERCUSSION_NOTE_MIN = 0xca;
		STATUS_PERCUSSION_NOTE_MAX = 0xdf;
	}

	EventMap[0x00] = EVENT_END;

	for (statusByte = 0x01; statusByte < STATUS_NOTE_MIN; statusByte++) {
		EventMap[statusByte] = EVENT_NOTE_PARAM;
	}

	for (statusByte = STATUS_NOTE_MIN; statusByte <= STATUS_NOTE_MAX; statusByte++) {
		EventMap[statusByte] = EVENT_NOTE;
	}

	EventMap[STATUS_NOTE_MAX + 1] = EVENT_TIE;
	EventMap[STATUS_NOTE_MAX + 2] = EVENT_REST;

	for (statusByte = STATUS_PERCUSSION_NOTE_MIN; statusByte <= STATUS_PERCUSSION_NOTE_MAX; statusByte++) {
		EventMap[statusByte] = EVENT_PERCUSSION_NOTE;
	}

	switch (version) {
	case NINSNES_EARLIER:
		EventMap[0xda] = EVENT_PROGCHANGE;
		EventMap[0xdb] = EVENT_PAN;
		EventMap[0xdc] = EVENT_PAN_FADE;
		EventMap[0xdd] = EVENT_PITCH_SLIDE;
		EventMap[0xde] = EVENT_VIBRATO_ON;
		EventMap[0xdf] = EVENT_VIBRATO_OFF;
		EventMap[0xe0] = EVENT_MASTER_VOLUME;
		EventMap[0xe1] = EVENT_MASTER_VOLUME_FADE;
		EventMap[0xe2] = EVENT_TEMPO;
		EventMap[0xe3] = EVENT_TEMPO_FADE;
		EventMap[0xe4] = EVENT_GLOBAL_TRANSPOSE;
		EventMap[0xe5] = EVENT_TREMOLO_ON;
		EventMap[0xe6] = EVENT_TREMOLO_OFF;
		EventMap[0xe7] = EVENT_VOLUME;
		EventMap[0xe8] = EVENT_VOLUME_FADE;
		EventMap[0xe9] = EVENT_CALL;
		EventMap[0xea] = EVENT_VIBRATO_FADE;
		EventMap[0xeb] = EVENT_PITCH_ENVELOPE_TO;
		EventMap[0xec] = EVENT_PITCH_ENVELOPE_FROM;
		//EventMap[0xed] = EVENT_PITCH_ENVELOPE_OFF;
		EventMap[0xee] = EVENT_TUNING;
		EventMap[0xef] = EVENT_ECHO_ON;
		EventMap[0xf0] = EVENT_ECHO_OFF;
		EventMap[0xf1] = EVENT_ECHO_PARAM;
		EventMap[0xf2] = EVENT_ECHO_VOLUME_FADE;

		if (volumeTable.empty()) {
			volumeTable.assign(std::begin(NINSNES_VOL_TABLE_EARLIER), std::end(NINSNES_VOL_TABLE_EARLIER));
		}

		if (durRateTable.empty()) {
			durRateTable.assign(std::begin(NINSNES_DUR_TABLE_EARLIER), std::end(NINSNES_DUR_TABLE_EARLIER));
		}

		if (panTable.empty()) {
			panTable.assign(std::begin(NINSNES_PAN_TABLE_EARLIER), std::end(NINSNES_PAN_TABLE_EARLIER));
		}

		break;

	case NINSNES_INTELLI_FE3:
		for (statusByte = 0x01; statusByte < STATUS_NOTE_MIN; statusByte++) {
			EventMap[statusByte] = EVENT_INTELLI_NOTE_PARAM;
		}

		// standard vcmds
		EventMap[0xd6] = EVENT_PROGCHANGE;
		EventMap[0xd7] = EVENT_PAN;
		EventMap[0xd8] = EVENT_PAN_FADE;
		EventMap[0xd9] = EVENT_VIBRATO_ON;
		EventMap[0xda] = EVENT_VIBRATO_OFF;
		EventMap[0xdb] = EVENT_MASTER_VOLUME;
		EventMap[0xdc] = EVENT_MASTER_VOLUME_FADE;
		EventMap[0xdd] = EVENT_TEMPO;
		EventMap[0xde] = EVENT_TEMPO_FADE;
		EventMap[0xdf] = EVENT_GLOBAL_TRANSPOSE;
		EventMap[0xe0] = EVENT_TRANSPOSE;
		EventMap[0xe1] = EVENT_TREMOLO_ON;
		EventMap[0xe2] = EVENT_TREMOLO_OFF;
		EventMap[0xe3] = EVENT_VOLUME;
		EventMap[0xe4] = EVENT_VOLUME_FADE;
		EventMap[0xe5] = EVENT_CALL;
		EventMap[0xe6] = EVENT_VIBRATO_FADE;
		EventMap[0xe7] = EVENT_PITCH_ENVELOPE_TO;
		EventMap[0xe8] = EVENT_PITCH_ENVELOPE_FROM;
		EventMap[0xe9] = EVENT_PITCH_ENVELOPE_OFF;
		EventMap[0xea] = EVENT_TUNING;
		EventMap[0xeb] = EVENT_ECHO_ON;
		EventMap[0xec] = EVENT_ECHO_OFF;
		EventMap[0xed] = EVENT_ECHO_PARAM;
		EventMap[0xee] = EVENT_ECHO_VOLUME_FADE;
		EventMap[0xef] = EVENT_PITCH_SLIDE;
		EventMap[0xf0] = EVENT_PERCCUSION_PATCH_BASE;

		EventMap[0xf1] = EVENT_UNKNOWN0;
		EventMap[0xf2] = EVENT_UNKNOWN0;
		EventMap[0xf3] = EVENT_UNKNOWN0;
		EventMap[0xf4] = EVENT_UNKNOWN0;
		EventMap[0xf5] = EVENT_INTELLI_FE3_UNKNOWN_EVENT_F5;
		EventMap[0xf6] = EVENT_UNKNOWN1;
		EventMap[0xf7] = EVENT_UNKNOWN1;
		EventMap[0xf8] = EVENT_INTELLI_JUMP_SHORT;
		EventMap[0xf9] = EVENT_INTELLI_FE3_UNKNOWN_EVENT_F9;
		EventMap[0xfa] = EVENT_INTELLI_FE3_UNKNOWN_EVENT_FA;
		EventMap[0xfb] = EVENT_UNKNOWN1;
		EventMap[0xfc] = EVENT_UNKNOWN2;
		EventMap[0xfd] = EVENT_UNKNOWN2;

		if (volumeTable.empty()) {
			volumeTable.assign(std::begin(NINSNES_VOL_TABLE_STANDARD), std::end(NINSNES_VOL_TABLE_STANDARD));
		}

		if (durRateTable.empty()) {
			durRateTable.assign(std::begin(NINSNES_DUR_TABLE_STANDARD), std::end(NINSNES_DUR_TABLE_STANDARD));
		}

		if (intelliDurVolTable.empty()) {
			intelliDurVolTable.assign(std::begin(NINSNES_DURVOL_TABLE_INTELLI_FE3), std::end(NINSNES_DURVOL_TABLE_INTELLI_FE3));
		}

		if (panTable.empty()) {
			panTable.assign(std::begin(NINSNES_PAN_TABLE_STANDARD), std::end(NINSNES_PAN_TABLE_STANDARD));
		}

		break;

	case NINSNES_INTELLI_FE4:
		for (statusByte = 0x01; statusByte < STATUS_NOTE_MIN; statusByte++) {
			EventMap[statusByte] = EVENT_INTELLI_NOTE_PARAM;
		}

		// standard vcmds
		EventMap[0xda] = EVENT_PROGCHANGE;
		EventMap[0xdb] = EVENT_PAN;
		EventMap[0xdc] = EVENT_PAN_FADE;
		EventMap[0xdd] = EVENT_VIBRATO_ON;
		EventMap[0xde] = EVENT_VIBRATO_OFF;
		EventMap[0xdf] = EVENT_MASTER_VOLUME;
		EventMap[0xe0] = EVENT_MASTER_VOLUME_FADE;
		EventMap[0xe1] = EVENT_TEMPO;
		EventMap[0xe2] = EVENT_TEMPO_FADE;
		EventMap[0xe3] = EVENT_GLOBAL_TRANSPOSE;
		EventMap[0xe4] = EVENT_TRANSPOSE;
		EventMap[0xe5] = EVENT_TREMOLO_ON;
		EventMap[0xe6] = EVENT_TREMOLO_OFF;
		EventMap[0xe7] = EVENT_VOLUME;
		EventMap[0xe8] = EVENT_VOLUME_FADE;
		EventMap[0xe9] = EVENT_CALL;
		EventMap[0xea] = EVENT_VIBRATO_FADE;
		EventMap[0xeb] = EVENT_PITCH_ENVELOPE_TO;
		EventMap[0xec] = EVENT_PITCH_ENVELOPE_FROM;
		EventMap[0xed] = EVENT_PITCH_ENVELOPE_OFF;
		EventMap[0xee] = EVENT_TUNING;
		EventMap[0xef] = EVENT_ECHO_ON;
		EventMap[0xf0] = EVENT_ECHO_OFF;
		EventMap[0xf1] = EVENT_ECHO_PARAM;
		EventMap[0xf2] = EVENT_ECHO_VOLUME_FADE;
		EventMap[0xf3] = EVENT_PITCH_SLIDE;
		EventMap[0xf4] = EVENT_PERCCUSION_PATCH_BASE;

		EventMap[0xf5] = EVENT_UNKNOWN0;
		EventMap[0xf6] = EVENT_UNKNOWN0;
		EventMap[0xf7] = EVENT_UNKNOWN1;
		EventMap[0xf8] = EventMap[0xf7];
		EventMap[0xf9] = EVENT_UNKNOWN0;
		EventMap[0xfa] = EVENT_INTELLI_FE3_UNKNOWN_EVENT_FA;
		EventMap[0xfb] = EVENT_UNKNOWN1;
		EventMap[0xfc] = EVENT_INTELLI_FE4_UNKNOWN_EVENT_FC;
		EventMap[0xfd] = EVENT_INTELLI_FE4_UNKNOWN_EVENT_FD;

		if (intelliDurVolTable.empty()) {
			intelliDurVolTable.assign(std::begin(NINSNES_DURVOL_TABLE_INTELLI_FE4), std::end(NINSNES_DURVOL_TABLE_INTELLI_FE4));
		}

		if (panTable.empty()) {
			panTable.assign(std::begin(NINSNES_PAN_TABLE_STANDARD), std::end(NINSNES_PAN_TABLE_STANDARD));
		}

		break;

	default: // NINSNES_STANDARD compatible versions
		EventMap[0xe0] = EVENT_PROGCHANGE;
		EventMap[0xe1] = EVENT_PAN;
		EventMap[0xe2] = EVENT_PAN_FADE;
		EventMap[0xe3] = EVENT_VIBRATO_ON;
		EventMap[0xe4] = EVENT_VIBRATO_OFF;
		EventMap[0xe5] = EVENT_MASTER_VOLUME;
		EventMap[0xe6] = EVENT_MASTER_VOLUME_FADE;
		EventMap[0xe7] = EVENT_TEMPO;
		EventMap[0xe8] = EVENT_TEMPO_FADE;
		EventMap[0xe9] = EVENT_GLOBAL_TRANSPOSE;
		EventMap[0xea] = EVENT_TRANSPOSE;
		EventMap[0xeb] = EVENT_TREMOLO_ON;
		EventMap[0xec] = EVENT_TREMOLO_OFF;
		EventMap[0xed] = EVENT_VOLUME;
		EventMap[0xee] = EVENT_VOLUME_FADE;
		EventMap[0xef] = EVENT_CALL;
		EventMap[0xf0] = EVENT_VIBRATO_FADE;
		EventMap[0xf1] = EVENT_PITCH_ENVELOPE_TO;
		EventMap[0xf2] = EVENT_PITCH_ENVELOPE_FROM;
		EventMap[0xf3] = EVENT_PITCH_ENVELOPE_OFF;
		EventMap[0xf4] = EVENT_TUNING;
		EventMap[0xf5] = EVENT_ECHO_ON;
		EventMap[0xf6] = EVENT_ECHO_OFF;
		EventMap[0xf7] = EVENT_ECHO_PARAM;
		EventMap[0xf8] = EVENT_ECHO_VOLUME_FADE;
		EventMap[0xf9] = EVENT_PITCH_SLIDE;
		EventMap[0xfa] = EVENT_PERCCUSION_PATCH_BASE;

		if (volumeTable.empty()) {
			volumeTable.assign(std::begin(NINSNES_VOL_TABLE_STANDARD), std::end(NINSNES_VOL_TABLE_STANDARD));
		}

		if (durRateTable.empty()) {
			durRateTable.assign(std::begin(NINSNES_DUR_TABLE_STANDARD), std::end(NINSNES_DUR_TABLE_STANDARD));
		}

		if (panTable.empty()) {
			panTable.assign(std::begin(NINSNES_PAN_TABLE_STANDARD), std::end(NINSNES_PAN_TABLE_STANDARD));
		}
	}

	// Modify mapping for derived versions
	switch (version) {
	case NINSNES_RD1:
		EventMap[0xfb] = EVENT_UNKNOWN2;
		EventMap[0xfc] = EVENT_UNKNOWN0;
		EventMap[0xfd] = EVENT_UNKNOWN0;
		EventMap[0xfe] = EVENT_UNKNOWN0;
		break;

	case NINSNES_RD2:
		EventMap[0xfb] = EVENT_RD2_PROGCHANGE_AND_ADSR;
		EventMap[0xfd] = EVENT_PROGCHANGE; // duplicated
		break;

	case NINSNES_KONAMI:
		EventMap[0xe4] = EVENT_UNKNOWN2;
		EventMap[0xe5] = EVENT_KONAMI_LOOP_START;
		EventMap[0xe6] = EVENT_KONAMI_LOOP_END;
		EventMap[0xe8] = EVENT_NOP;
		EventMap[0xe9] = EVENT_NOP;
		EventMap[0xf5] = EVENT_UNKNOWN0;
		EventMap[0xf6] = EVENT_UNKNOWN0;
		EventMap[0xf7] = EVENT_UNKNOWN0;
		EventMap[0xf8] = EVENT_UNKNOWN0;
		EventMap[0xfb] = EVENT_KONAMI_ADSR_AND_GAIN;
		EventMap[0xfc] = EVENT_NOP;
		EventMap[0xfd] = EVENT_NOP;
		EventMap[0xfe] = EVENT_NOP;
		break;

	case NINSNES_LEMMINGS:
		for (statusByte = 0x01; statusByte < STATUS_NOTE_MIN; statusByte++) {
			EventMap[statusByte] = EVENT_LEMMINGS_NOTE_PARAM;
		}

		EventMap[0xe5] = EVENT_UNKNOWN1; // master volume NYI?
		EventMap[0xe6] = EVENT_UNKNOWN2; // master volume fade?
		EventMap[0xfb] = EVENT_NOP1;
		EventMap[0xfc] = EVENT_UNKNOWN0;
		EventMap[0xfd] = EVENT_UNKNOWN0;
		EventMap[0xfe] = EVENT_UNKNOWN0;

		volumeTable.clear();
		durRateTable.clear();
		break;

	case NINSNES_HUMAN:
		break;

	case NINSNES_TOSE:
		panTable.clear();
		break;
	}
}

double NinSnesSeq::GetTempoInBPM(uint8_t tempo)
{
	if (tempo != 0)
	{
		return (double)60000000 / (SEQ_PPQN * 2000) * ((double)tempo / 256);
	}
	else
	{
		return 1.0; // since tempo 0 cannot be expressed, this function returns a very small value.
	}
}

uint16_t NinSnesSeq::ConvertToAPUAddress(uint16_t offset)
{
	if (version == NINSNES_KONAMI) {
		return konamiBaseAddress + offset;
	}
	else {
		return offset;
	}
}

uint16_t NinSnesSeq::GetShortAddress(uint32_t offset)
{
	return ConvertToAPUAddress(GetShort(offset));
}

//  **************
//  NinSnesSection
//  **************

NinSnesSection::NinSnesSection(NinSnesSeq* parentFile, long offset, long length)
	: VGMSeqSection(parentFile, offset, length)
{
}

bool NinSnesSection::GetTrackPointers()
{
	NinSnesSeq* parentSeq = (NinSnesSeq*)this->parentSeq;
	uint32_t curOffset = dwOffset;

	VGMHeader* header = AddHeader(curOffset, 16);
	uint8_t numActiveTracks = 0;
	for (int trackIndex = 0; trackIndex < MAX_TRACKS; trackIndex++) {
		if (curOffset + 1 >= 0x10000) {
			return false;
		}

		uint16_t startAddress = GetShort(curOffset);

		bool active = ((startAddress & 0xff00) != 0);
		NinSnesTrack* track;
		if (active) {
			startAddress = ConvertToAPUAddress(startAddress);

			// correct sequence address
			// probably it's not necessary for regular case, but just in case...
			if (startAddress < dwOffset) {
				uint32_t distance = dwOffset - startAddress;
				dwOffset = startAddress;
				if (unLength != 0) {
					unLength += distance;
				}
			}

			std::wstringstream trackName;
			trackName << L"Track " << (trackIndex + 1);
			track = new NinSnesTrack(this, startAddress, 0, trackName.str());

			numActiveTracks++;
		}
		else {
			// add an inactive track
			track = new NinSnesTrack(this, curOffset, 2, L"NULL");
			track->available = false;
		}
		track->shared = &parentSeq->sharedTrackData[trackIndex];
		aTracks.push_back(track);

		wchar_t name[32];
		wsprintf(name, L"Track Pointer #%d", trackIndex + 1);

		header->AddSimpleItem(curOffset, 2, name);
		curOffset += 2;
	}

	if (numActiveTracks == 0) {
		return false;
	}

	return true;
}

uint16_t NinSnesSection::ConvertToAPUAddress(uint16_t offset)
{
	NinSnesSeq* parentSeq = (NinSnesSeq*)this->parentSeq;
	return parentSeq->ConvertToAPUAddress(offset);
}

uint16_t NinSnesSection::GetShortAddress(uint32_t offset)
{
	NinSnesSeq* parentSeq = (NinSnesSeq*)this->parentSeq;
	return parentSeq->GetShortAddress(offset);
}

NinSnesTrackSharedData::NinSnesTrackSharedData()
{
	ResetVars();
}

void NinSnesTrackSharedData::ResetVars(void)
{
	loopCount = 0;
	spcTranspose = 0;

	// just in case
	spcNoteDuration = 1;
	spcNoteDurRate = 0xfc;
	spcNoteVolume = 0xfc;

	// Konami:
	konamiLoopStart = 0;
	konamiLoopCount = 0;

	// Intelligent Systems:
	intelliFE3NoteFlags = NINSNES_INTELLI_FE3FLAGS_NEW_NOTE_PARAM;
}

//  ************
//  NinSnesTrack
//  ************

NinSnesTrack::NinSnesTrack(NinSnesSection* parentSection, long offset, long length, const std::wstring& theName)
	: SeqTrack(parentSection->parentSeq, offset, length, theName),
	parentSection(parentSection),
	shared(NULL),
	available(true)
{
	ResetVars();
	bDetermineTrackLengthEventByEvent = true;
}

void NinSnesTrack::ResetVars(void)
{
	SeqTrack::ResetVars();

	cKeyCorrection = SEQ_KEYOFS;
	if (shared != NULL) {
		transpose = shared->spcTranspose;
	}
}

bool NinSnesTrack::ReadEvent(void)
{
	if (!available) {
		return false;
	}

	NinSnesSeq* parentSeq = (NinSnesSeq*)this->parentSeq;
	uint32_t beginOffset = curOffset;
	if (curOffset >= 0x10000)
	{
		return false;
	}

	uint8_t statusByte = GetByte(curOffset++);
	bool bContinue = true;

	std::wstringstream desc;

	NinSnesSeqEventType eventType = (NinSnesSeqEventType)0;
	std::map<uint8_t, NinSnesSeqEventType>::iterator pEventType = parentSeq->EventMap.find(statusByte);
	if (pEventType != parentSeq->EventMap.end()) {
		eventType = pEventType->second;
	}

	switch (eventType)
	{
	case EVENT_UNKNOWN0:
	{
		desc << L"Event: 0x" << std::hex << std::setfill(L'0') << std::setw(2) << std::uppercase << (int)statusByte;
		AddUnknown(beginOffset, curOffset - beginOffset, L"Unknown Event", desc.str().c_str());
		break;
	}

	case EVENT_UNKNOWN1:
	{
		uint8_t arg1 = GetByte(curOffset++);
		desc << L"Event: 0x" << std::hex << std::setfill(L'0') << std::setw(2) << std::uppercase << (int)statusByte
			<< std::dec << std::setfill(L' ') << std::setw(0)
			<< L"  Arg1: " << (int)arg1;
		AddUnknown(beginOffset, curOffset - beginOffset, L"Unknown Event", desc.str().c_str());
		break;
	}

	case EVENT_UNKNOWN2:
	{
		uint8_t arg1 = GetByte(curOffset++);
		uint8_t arg2 = GetByte(curOffset++);
		desc << L"Event: 0x" << std::hex << std::setfill(L'0') << std::setw(2) << std::uppercase << (int)statusByte
			<< std::dec << std::setfill(L' ') << std::setw(0)
			<< L"  Arg1: " << (int)arg1
			<< L"  Arg2: " << (int)arg2;
		AddUnknown(beginOffset, curOffset - beginOffset, L"Unknown Event", desc.str().c_str());
		break;
	}

	case EVENT_UNKNOWN3:
	{
		uint8_t arg1 = GetByte(curOffset++);
		uint8_t arg2 = GetByte(curOffset++);
		uint8_t arg3 = GetByte(curOffset++);
		desc << L"Event: 0x" << std::hex << std::setfill(L'0') << std::setw(2) << std::uppercase << (int)statusByte
			<< std::dec << std::setfill(L' ') << std::setw(0)
			<< L"  Arg1: " << (int)arg1
			<< L"  Arg2: " << (int)arg2
			<< L"  Arg3: " << (int)arg3;
		AddUnknown(beginOffset, curOffset - beginOffset, L"Unknown Event", desc.str().c_str());
		break;
	}

	case EVENT_UNKNOWN4:
	{
		uint8_t arg1 = GetByte(curOffset++);
		uint8_t arg2 = GetByte(curOffset++);
		uint8_t arg3 = GetByte(curOffset++);
		uint8_t arg4 = GetByte(curOffset++);
		desc << L"Event: 0x" << std::hex << std::setfill(L'0') << std::setw(2) << std::uppercase << (int)statusByte
			<< std::dec << std::setfill(L' ') << std::setw(0)
			<< L"  Arg1: " << (int)arg1
			<< L"  Arg2: " << (int)arg2
			<< L"  Arg3: " << (int)arg3
			<< L"  Arg4: " << (int)arg4;
		AddUnknown(beginOffset, curOffset - beginOffset, L"Unknown Event", desc.str().c_str());
		break;
	}

	case EVENT_NOP:
	{
		AddGenericEvent(beginOffset, curOffset - beginOffset, L"NOP", desc.str().c_str(), CLR_MISC, ICON_BINARY);
		break;
	}

	case EVENT_NOP1:
	{
		curOffset++;
		AddGenericEvent(beginOffset, curOffset - beginOffset, L"NOP", desc.str().c_str(), CLR_MISC, ICON_BINARY);
		break;
	}

	case EVENT_END:
	{
		// AddEvent is called at the last of this function

		if (shared->loopCount == 0) {
			// finish this section as soon as possible
			if (readMode == READMODE_FIND_DELTA_LENGTH) {
				for (size_t trackIndex = 0; trackIndex < parentSeq->aTracks.size(); trackIndex++) {
					parentSeq->aTracks[trackIndex]->deltaLength = GetTime();
				}
			}
			else if (readMode == READMODE_CONVERT_TO_MIDI) {
				// TODO: cancel all expected notes and fader-output events
				for (size_t trackIndex = 0; trackIndex < parentSeq->aTracks.size(); trackIndex++) {
					parentSeq->aTracks[trackIndex]->LimitPrevDurNoteEnd();
				}
			}

			parentSeq->InactivateAllTracks();
			bContinue = false;
		}
		else {
			uint32_t eventLength = curOffset - beginOffset;

			shared->loopCount--;
			if (shared->loopCount == 0) {
				// repeat end
				curOffset = shared->loopReturnAddress;
			}
			else {
				// repeat again
				curOffset = shared->loopStartAddress;
			}
		}
		break;
	}

	case EVENT_NOTE_PARAM:
	{
	OnEventNoteParam:
		// param #0: duration
		shared->spcNoteDuration = statusByte;
		desc << L"Duration: " << (int)shared->spcNoteDuration;

		// param #1: quantize and velocity (optional)
		if (curOffset + 1 < 0x10000 && GetByte(curOffset) <= 0x7f) {
			uint8_t quantizeAndVelocity = GetByte(curOffset++);

			uint8_t durIndex = (quantizeAndVelocity >> 4) & 7;
			uint8_t velIndex = quantizeAndVelocity & 15;

			shared->spcNoteDurRate = parentSeq->durRateTable[durIndex];
			shared->spcNoteVolume = parentSeq->volumeTable[velIndex];

			desc << L"  Quantize: " << (int)durIndex << L" (" << (int)shared->spcNoteDurRate << L"/256)" << L"  Velocity: " << (int)velIndex << L" (" << (int)shared->spcNoteVolume << L"/256)";
		}

		AddGenericEvent(beginOffset, curOffset - beginOffset, L"Note Param", desc.str().c_str(), CLR_DURNOTE, ICON_CONTROL);
		break;
	}

	case EVENT_NOTE:
	{
		uint8_t noteNumber = statusByte - parentSeq->STATUS_NOTE_MIN;
		uint8_t duration = (shared->spcNoteDuration * shared->spcNoteDurRate) >> 8;
		duration = min(max(duration, 1), shared->spcNoteDuration - 2);

		// Note: Konami engine can have volume=0
		AddNoteByDur(beginOffset, curOffset - beginOffset, noteNumber, shared->spcNoteVolume / 2, duration, L"Note");
		AddTime(shared->spcNoteDuration);
		break;
	}

	case EVENT_TIE:
	{
		uint8_t duration = (shared->spcNoteDuration * shared->spcNoteDurRate) >> 8;
		duration = min(max(duration, 1), shared->spcNoteDuration - 2);
		desc << L"Duration: " << (int)duration;
		MakePrevDurNoteEnd(GetTime() + duration);
		AddGenericEvent(beginOffset, curOffset - beginOffset, L"Tie", desc.str().c_str(), CLR_TIE);
		AddTime(shared->spcNoteDuration);
		break;
	}

	case EVENT_REST:
		AddRest(beginOffset, curOffset - beginOffset, shared->spcNoteDuration);
		break;

	case EVENT_PERCUSSION_NOTE:
	{
		uint8_t noteNumber = statusByte - parentSeq->STATUS_PERCUSSION_NOTE_MIN; // + percussion base
		uint8_t duration = (shared->spcNoteDuration * shared->spcNoteDurRate) >> 8;
		duration = min(max(duration, 1), shared->spcNoteDuration - 2);

		// Note: Konami engine can have volume=0
		AddPercNoteByDur(beginOffset, curOffset - beginOffset, noteNumber, shared->spcNoteVolume / 2, duration, L"Percussion Note");
		AddTime(shared->spcNoteDuration);
		break;
	}

	case EVENT_PROGCHANGE:
	{
		uint8_t newProgNum = GetByte(curOffset++);
		if (parentSeq->version != NINSNES_HUMAN) {
			if (newProgNum >= 0x80) {
				// standard engine does nothing special, but Star Fox does
				newProgNum = (newProgNum - parentSeq->STATUS_PERCUSSION_NOTE_MIN) + parentSeq->spcPercussionBase;
			}
		}
		AddProgramChange(beginOffset, curOffset - beginOffset, newProgNum, true);
		break;
	}

	case EVENT_PAN:
	{
		uint8_t newPan = GetByte(curOffset++);

		uint8_t panIndex = newPan;
		bool reverseLeft = false;
		bool reverseRight = false;
		if (parentSeq->version != NINSNES_TOSE) {
			panIndex &= 0x1f;
			reverseLeft = (newPan & 0x80) != 0;
			reverseRight = (newPan & 0x40) != 0;
		}

		double volumeLeft;
		double volumeRight;
		GetVolumeBalance(panIndex << 8, volumeLeft, volumeRight);

		double linearPan = (double)volumeRight / (volumeLeft + volumeRight);
		double volumeScale;
		double midiScalePan = ConvertPercentPanToStdMidiScale(linearPan, &volumeScale);
		volumeScale /= volumeLeft + volumeRight;
		volumeScale = min(max(volumeScale, 0.0), 1.0);

		int8_t midiPan;
		if (midiScalePan == 0.0) {
			midiPan = 0;
		}
		else {
			midiPan = 1 + roundi(midiScalePan * 126.0);
		}

		AddPan(beginOffset, curOffset - beginOffset, midiPan);
		AddExpressionNoItem(roundi(sqrt(volumeScale) * 127.0));
		break;
	}

	case EVENT_PAN_FADE:
	{
		uint8_t fadeLength = GetByte(curOffset++);
		uint8_t newPan = GetByte(curOffset++);

		double volumeLeft;
		double volumeRight;
		GetVolumeBalance(newPan << 8, volumeLeft, volumeRight);

		double linearPan = (double)volumeRight / (volumeLeft + volumeRight);
		double midiScalePan = ConvertPercentPanToStdMidiScale(linearPan);

		int8_t midiPan;
		if (midiScalePan == 0.0) {
			midiPan = 0;
		}
		else {
			midiPan = 1 + roundi(midiScalePan * 126.0);
		}

		// TODO: fade in real curve
		// TODO: apply volume scale
		AddPanSlide(beginOffset, curOffset - beginOffset, fadeLength, midiPan);
		break;
	}

	case EVENT_VIBRATO_ON:
	{
		uint8_t vibratoDelay = GetByte(curOffset++);
		uint8_t vibratoRate = GetByte(curOffset++);
		uint8_t vibratoDepth = GetByte(curOffset++);

		desc << L"Delay: " << (int)vibratoDelay << L"  Rate: " << (int)vibratoRate << L"  Depth: " << (int)vibratoDepth;
		AddGenericEvent(beginOffset, curOffset - beginOffset, L"Vibrato", desc.str().c_str(), CLR_MODULATION, ICON_CONTROL);
		break;
	}

	case EVENT_VIBRATO_OFF:
	{
		AddGenericEvent(beginOffset, curOffset - beginOffset, L"Vibrato Off", desc.str().c_str(), CLR_MODULATION, ICON_CONTROL);
		break;
	}

	case EVENT_MASTER_VOLUME:
	{
		uint8_t newVol = GetByte(curOffset++);
		AddMasterVol(beginOffset, curOffset - beginOffset, newVol / 2);
		break;
	}

	case EVENT_MASTER_VOLUME_FADE:
	{
		uint8_t fadeLength = GetByte(curOffset++);
		uint8_t newVol = GetByte(curOffset++);

		desc << L"Length: " << (int)fadeLength << L"  Volume: " << (int)newVol;
		AddMastVolSlide(beginOffset, curOffset - beginOffset, fadeLength, newVol / 2);
		break;
	}

	case EVENT_TEMPO:
	{
		uint8_t newTempo = GetByte(curOffset++);
		AddTempoBPM(beginOffset, curOffset - beginOffset, parentSeq->GetTempoInBPM(newTempo));
		break;
	}

	case EVENT_TEMPO_FADE:
	{
		uint8_t fadeLength = GetByte(curOffset++);
		uint8_t newTempo = GetByte(curOffset++);

		AddTempoSlide(beginOffset, curOffset - beginOffset, fadeLength, (int)(60000000 / parentSeq->GetTempoInBPM(newTempo)));
		break;
	}

	case EVENT_GLOBAL_TRANSPOSE:
	{
		int8_t semitones = GetByte(curOffset++);
		AddGlobalTranspose(beginOffset, curOffset - beginOffset, semitones);
		break;
	}

	case EVENT_TRANSPOSE:
	{
		int8_t semitones = GetByte(curOffset++);
		shared->spcTranspose = semitones;
		AddTranspose(beginOffset, curOffset - beginOffset, semitones);
		break;
	}

	case EVENT_TREMOLO_ON:
	{
		uint8_t tremoloDelay = GetByte(curOffset++);
		uint8_t tremoloRate = GetByte(curOffset++);
		uint8_t tremoloDepth = GetByte(curOffset++);

		desc << L"Delay: " << (int)tremoloDelay << L"  Rate: " << (int)tremoloRate << L"  Depth: " << (int)tremoloDepth;
		AddGenericEvent(beginOffset, curOffset - beginOffset, L"Tremolo", desc.str().c_str(), CLR_MODULATION, ICON_CONTROL);
		break;
	}

	case EVENT_TREMOLO_OFF:
	{
		AddGenericEvent(beginOffset, curOffset - beginOffset, L"Tremolo Off", desc.str().c_str(), CLR_MODULATION, ICON_CONTROL);
		break;
	}

	case EVENT_VOLUME:
	{
		uint8_t newVol = GetByte(curOffset++);
		AddVol(beginOffset, curOffset - beginOffset, newVol / 2);
		break;
	}

	case EVENT_VOLUME_FADE:
	{
		uint8_t fadeLength = GetByte(curOffset++);
		uint8_t newVol = GetByte(curOffset++);
		AddVolSlide(beginOffset, curOffset - beginOffset, fadeLength, newVol / 2);
		break;
	}

	case EVENT_CALL:
	{
		uint16_t dest = GetShortAddress(curOffset); curOffset += 2;
		uint8_t times = GetByte(curOffset++);

		shared->loopReturnAddress = curOffset;
		shared->loopStartAddress = dest;
		shared->loopCount = times;

		desc << L"Destination: $" << std::hex << std::setfill(L'0') << std::setw(4) << std::uppercase << (int)dest
			<< std::dec << std::setfill(L' ') << std::setw(0) << L"  Times: " << (int)times;
		AddGenericEvent(beginOffset, curOffset - beginOffset, L"Pattern Play", desc.str().c_str(), CLR_LOOP, ICON_STARTREP);

		// Add the next "END" event to UI
		if (curOffset < 0x10000 && GetByte(curOffset) == parentSeq->STATUS_END) {
			if (shared->loopCount == 0) {
				AddGenericEvent(curOffset, 1, L"Section End", desc.str().c_str(), CLR_TRACKEND, ICON_TRACKEND);
			}
			else {
				AddGenericEvent(curOffset, 1, L"Pattern End", desc.str().c_str(), CLR_TRACKEND, ICON_TRACKEND);
			}
		}

		curOffset = shared->loopStartAddress;
		break;
	}

	case EVENT_VIBRATO_FADE:
	{
		uint8_t fadeLength = GetByte(curOffset++);
		desc << L"Length: " << (int)fadeLength;
		AddGenericEvent(beginOffset, curOffset - beginOffset, L"Vibrato Fade", desc.str().c_str(), CLR_MODULATION, ICON_CONTROL);
		break;
	}

	case EVENT_PITCH_ENVELOPE_TO:
	{
		uint8_t pitchEnvDelay = GetByte(curOffset++);
		uint8_t pitchEnvLength = GetByte(curOffset++);
		int8_t pitchEnvSemitones = (int8_t)GetByte(curOffset++);

		desc << L"Delay: " << (int)pitchEnvDelay << L"  Length: " << (int)pitchEnvLength << L"  Semitones: " << (int)pitchEnvSemitones;
		AddGenericEvent(beginOffset, curOffset - beginOffset, L"Pitch Envelope (To)", desc.str().c_str(), CLR_PITCHBEND, ICON_CONTROL);
		break;
	}

	case EVENT_PITCH_ENVELOPE_FROM:
	{
		uint8_t pitchEnvDelay = GetByte(curOffset++);
		uint8_t pitchEnvLength = GetByte(curOffset++);
		int8_t pitchEnvSemitones = (int8_t)GetByte(curOffset++);

		desc << L"Delay: " << (int)pitchEnvDelay << L"  Length: " << (int)pitchEnvLength << L"  Semitones: " << (int)pitchEnvSemitones;
		AddGenericEvent(beginOffset, curOffset - beginOffset, L"Pitch Envelope (From)", desc.str().c_str(), CLR_PITCHBEND, ICON_CONTROL);
		break;
	}

	case EVENT_PITCH_ENVELOPE_OFF:
	{
		AddGenericEvent(beginOffset, curOffset - beginOffset, L"Pitch Envelope Off", desc.str().c_str(), CLR_PITCHBEND, ICON_CONTROL);
		break;
	}

	case EVENT_TUNING:
	{
		uint8_t newTuning = GetByte(curOffset++);
		AddFineTuning(beginOffset, curOffset - beginOffset, (newTuning / 256.0) * 100.0);
		break;
	}

	case EVENT_ECHO_ON:
	{
		uint8_t spcEON = GetByte(curOffset++);
		uint8_t spcEVOL_L = GetByte(curOffset++);
		uint8_t spcEVOL_R = GetByte(curOffset++);

		desc << L"Channels: ";
		for (int channelNo = MAX_TRACKS - 1; channelNo >= 0; channelNo--) {
			if ((spcEON & (1 << channelNo)) != 0) {
				desc << (int)channelNo;
				parentSeq->aTracks[channelNo]->AddReverbNoItem(40);
			}
			else {
				desc << L"-";
				parentSeq->aTracks[channelNo]->AddReverbNoItem(0);
			}
		}

		desc << L"  Volume Left: " << (int)spcEVOL_L << L"  Volume Right: " << (int)spcEVOL_R;
		AddGenericEvent(beginOffset, curOffset - beginOffset, L"Echo", desc.str().c_str(), CLR_REVERB, ICON_CONTROL);
		break;
	}

	case EVENT_ECHO_OFF:
	{
		AddGenericEvent(beginOffset, curOffset - beginOffset, L"Echo Off", desc.str().c_str(), CLR_REVERB, ICON_CONTROL);
		break;
	}

	case EVENT_ECHO_PARAM:
	{
		uint8_t spcEDL = GetByte(curOffset++);
		uint8_t spcEFB = GetByte(curOffset++);
		uint8_t spcFIR = GetByte(curOffset++);

		desc << L"Delay: " << (int)spcEDL << L"  Feedback: " << (int)spcEFB << L"  FIR: " << (int)spcFIR;
		AddGenericEvent(beginOffset, curOffset - beginOffset, L"Echo Off", desc.str().c_str(), CLR_REVERB, ICON_CONTROL);
		break;
	}

	case EVENT_ECHO_VOLUME_FADE:
	{
		uint8_t fadeLength = GetByte(curOffset++);
		uint8_t spcEVOL_L = GetByte(curOffset++);
		uint8_t spcEVOL_R = GetByte(curOffset++);

		desc << L"Length: " << (int)fadeLength << L"  Volume Left: " << (int)spcEVOL_L << L"  Volume Right: " << (int)spcEVOL_R;
		AddGenericEvent(beginOffset, curOffset - beginOffset, L"Echo Volume Fade", desc.str().c_str(), CLR_REVERB, ICON_CONTROL);
		break;
	}

	case EVENT_PITCH_SLIDE:
	{
		uint8_t pitchSlideDelay = GetByte(curOffset++);
		uint8_t pitchSlideLength = GetByte(curOffset++);
		uint8_t pitchSlideTargetNote = GetByte(curOffset++);

		desc << L"Delay: " << (int)pitchSlideDelay << L"  Length: " << (int)pitchSlideLength << L"  Note: " << (int)(pitchSlideTargetNote - 0x80);
		AddGenericEvent(beginOffset, curOffset - beginOffset, L"Pitch Slide", desc.str().c_str(), CLR_PITCHBEND, ICON_CONTROL);
		break;
	}

	case EVENT_PERCCUSION_PATCH_BASE:
	{
		uint8_t percussionBase = GetByte(curOffset++);
		parentSeq->spcPercussionBase = percussionBase;

		desc << L"Percussion Base: " << (int)percussionBase;
		AddGenericEvent(beginOffset, curOffset - beginOffset, L"Percussion Base", desc.str().c_str(), CLR_CHANGESTATE, ICON_CONTROL);
		break;
	}

	// NINTENDO RD2 EVENTS START >>

	case EVENT_RD2_PROGCHANGE_AND_ADSR:
	{
		// This event overwrites ADSR in instrument table
		uint8_t newProgNum = GetByte(curOffset++);
		uint8_t adsr1 = GetByte(curOffset++);
		uint8_t adsr2 = GetByte(curOffset++);
		AddProgramChange(beginOffset, curOffset - beginOffset, newProgNum, true, L"Program Change & ADSR");
		break;
	}

	// << NINTENDO RD2 EVENTS END

	// KONAMI EVENTS START >>

	case EVENT_KONAMI_LOOP_START:
	{
		AddGenericEvent(beginOffset, curOffset - beginOffset, L"Loop Start", desc.str().c_str(), CLR_LOOP, ICON_STARTREP);
		shared->konamiLoopStart = curOffset;
		shared->konamiLoopCount = 0;
		break;
	}

	case EVENT_KONAMI_LOOP_END:
	{
		uint8_t times = GetByte(curOffset++);
		int8_t volumeDelta = GetByte(curOffset++);
		int8_t pitchDelta = GetByte(curOffset++);

		desc << L"Times: " << (int)times << L"  Volume Delta: " << (int)volumeDelta << L"  Pitch Delta: " << (int)pitchDelta << L"/16 semitones";
		AddGenericEvent(beginOffset, curOffset - beginOffset, L"Loop End", desc.str().c_str(), CLR_LOOP, ICON_ENDREP);

		shared->konamiLoopCount++;
		if (shared->konamiLoopCount != times) {
			// repeat again
			curOffset = shared->konamiLoopStart;
		}

		break;
	}

	case EVENT_KONAMI_ADSR_AND_GAIN:
	{
		uint8_t adsr1 = GetByte(curOffset++);
		uint8_t adsr2 = GetByte(curOffset++);
		uint8_t gain = GetByte(curOffset++);
		desc << std::hex << std::setfill(L'0') << std::setw(2) << std::uppercase << L"ADSR(1): $" << adsr1 << L"  ADSR(2): $" << adsr2 << L"  GAIN: $" << gain;
		AddGenericEvent(beginOffset, curOffset - beginOffset, L"ADSR/GAIN", desc.str(), CLR_ADSR, ICON_CONTROL);
		break;
	}

	// << KONAMI EVENTS END

	// LEMMINGS EVENTS START >>

	case EVENT_LEMMINGS_NOTE_PARAM:
	{
		// param #0: duration
		shared->spcNoteDuration = statusByte;
		desc << L"Duration: " << (int)shared->spcNoteDuration;

		// param #1: quantize (optional)
		if (curOffset + 1 < 0x10000 && GetByte(curOffset) <= 0x7f) {
			uint8_t durByte = GetByte(curOffset++);
			shared->spcNoteDurRate = (durByte << 1) + (durByte >> 1) + (durByte & 1); // approx percent?
			desc << L"  Quantize: " << durByte << L" (" << shared->spcNoteDurRate << L"/256)";

			// param #2: velocity (optional)
			if (curOffset + 1 < 0x10000 && GetByte(curOffset) <= 0x7f) {
				uint8_t velByte = GetByte(curOffset++);
				shared->spcNoteVolume = velByte << 1;
				desc << L"  Velocity: " << velByte << L" (" << shared->spcNoteVolume << L"/256)";
			}
		}

		AddGenericEvent(beginOffset, curOffset - beginOffset, L"Note Param", desc.str().c_str(), CLR_DURNOTE, ICON_CONTROL);
		break;
	}

	// << LEMMINGS EVENTS END

	// INTELLIGENT SYSTEMS EVENTS START >>

	case EVENT_INTELLI_NOTE_PARAM:
	{
		if (parentSeq->version == NINSNES_INTELLI_FE3 &&
			(shared->intelliFE3NoteFlags & NINSNES_INTELLI_FE3FLAGS_NEW_NOTE_PARAM) == 0) {
			goto OnEventNoteParam;
		}

		// param #0: duration
		shared->spcNoteDuration = statusByte;
		desc << L"Duration: " << (int)shared->spcNoteDuration;

		// param #1,2...: quantize/velocity (optional)
		while (curOffset + 1 < 0x10000 && GetByte(curOffset) <= 0x7f) {
			uint8_t noteParam = GetByte(curOffset++);
			if (noteParam < 0x40) { // 00..3f
				uint8_t durIndex = noteParam & 0x3f;
				shared->spcNoteDurRate = parentSeq->intelliDurVolTable[durIndex];
				desc << L"  Quantize: " << durIndex << L" (" << shared->spcNoteDurRate << L"/256)";
			}
			else { // 40..7f
				uint8_t velIndex = noteParam & 0x3f;
				shared->spcNoteVolume = parentSeq->intelliDurVolTable[velIndex];
				desc << L"  Velocity: " << velIndex << L" (" << shared->spcNoteVolume << L"/256)";
			}
		}

		AddGenericEvent(beginOffset, curOffset - beginOffset, L"Note Param", desc.str().c_str(), CLR_DURNOTE, ICON_CONTROL);
		break;
	}

	case EVENT_INTELLI_JUMP_SHORT:
	{
		uint8_t offset = GetByte(curOffset++);
		uint16_t dest = curOffset + offset;

		desc << L"Destination: $" << std::hex << std::setfill(L'0') << std::setw(4) << std::uppercase << (int)dest;
		AddGenericEvent(beginOffset, curOffset - beginOffset, L"Jump (Short)", desc.str().c_str(), CLR_MISC);

		curOffset = dest;
		break;
	}

	case EVENT_INTELLI_FE3_UNKNOWN_EVENT_F5:
	{
		int8_t param = GetByte(curOffset++);
		if (param >= 0) {
			uint8_t bit = 1 << (param & 7);
			if (param & 8) {
				shared->intelliFE3NoteFlags |= bit;
			}
			else {
				shared->intelliFE3NoteFlags &= ~bit;
			}
		}
		AddUnknown(beginOffset, curOffset - beginOffset);
		break;
	}

	case EVENT_INTELLI_FE3_UNKNOWN_EVENT_F9:
	{
		curOffset += 36;
		AddUnknown(beginOffset, curOffset - beginOffset);
		break;
	}

	case EVENT_INTELLI_FE3_UNKNOWN_EVENT_FA:
	{
		int8_t arg1 = GetByte(curOffset++);
		if (arg1 >= 0) {
			curOffset += arg1 * 4;
		}
		else {
			if (parentSeq->version == NINSNES_INTELLI_FE3) {
				curOffset += 6;
			}
		}
		AddUnknown(beginOffset, curOffset - beginOffset);
		break;
	}

	case EVENT_INTELLI_FE4_UNKNOWN_EVENT_FC:
	{
		uint8_t arg1 = GetByte(curOffset++);
		curOffset += ((arg1 & 15) + 1) * 3;
		AddUnknown(beginOffset, curOffset - beginOffset);
		break;
	}

	case EVENT_INTELLI_FE4_UNKNOWN_EVENT_FD:
	{
		uint8_t type = GetByte(curOffset++);

		switch (type) {
		case 0x01:
			curOffset++;
			break;

		case 0x02:
			curOffset++;
			break;
		}

		AddUnknown(beginOffset, curOffset - beginOffset);
		break;
	}

	// << INTELLIGENT SYSTEMS EVENTS END

	default:
		desc << L"Event: 0x" << std::hex << std::setfill(L'0') << std::setw(2) << std::uppercase << (int)statusByte;
		AddUnknown(beginOffset, curOffset - beginOffset, L"Unknown Event", desc.str().c_str());
		pRoot->AddLogItem(new LogItem(std::wstring(L"Unknown Event - ") + desc.str(), LOG_LEVEL_ERR, std::wstring(L"AkaoSnesSeq")));
		bContinue = false;
		break;
	}

	// Add the next "END" event to UI
	// (because it often gets interrupted by the end of other track)
	if (curOffset + 1 <= 0x10000 && statusByte != parentSeq->STATUS_END && GetByte(curOffset) == parentSeq->STATUS_END) {
		if (shared->loopCount == 0) {
			AddGenericEvent(curOffset, 1, L"Section End", desc.str().c_str(), CLR_TRACKEND, ICON_TRACKEND);
		}
		else {
			AddGenericEvent(curOffset, 1, L"Pattern End", desc.str().c_str(), CLR_TRACKEND, ICON_TRACKEND);
		}
	}

	return bContinue;
}

uint16_t NinSnesTrack::ConvertToAPUAddress(uint16_t offset)
{
	NinSnesSeq* parentSeq = (NinSnesSeq*)this->parentSeq;
	return parentSeq->ConvertToAPUAddress(offset);
}

uint16_t NinSnesTrack::GetShortAddress(uint32_t offset)
{
	NinSnesSeq* parentSeq = (NinSnesSeq*)this->parentSeq;
	return parentSeq->GetShortAddress(offset);
}

void NinSnesTrack::GetVolumeBalance(uint16_t pan, double & volumeLeft, double & volumeRight)
{
	NinSnesSeq* parentSeq = (NinSnesSeq*)this->parentSeq;

	uint8_t panIndex = pan >> 8;
	if (parentSeq->version == NINSNES_TOSE) {
		if (panIndex <= 10) {
			// pan right, decrease left volume
			volumeLeft = (255 - 25 * max(10 - panIndex, 0)) / 256.0;
			volumeRight = 1.0;
		}
		else {
			// pan left, decrease right volume
			volumeLeft = 1.0;
			volumeRight = (255 - 25 * max(panIndex - 10, 0)) / 256.0;
		}
	}
	else {
		uint8_t panMaxIndex = parentSeq->panTable.size() - 1;
		if (panIndex > panMaxIndex) {
			// unexpected behavior
			pan = panMaxIndex << 8;
			panIndex = panMaxIndex;
		}

		// actual engine divides pan by 256, though pan value is 7-bit
		volumeLeft = ReadPanTable((panMaxIndex << 8) - pan) / 128.0;
		volumeRight = ReadPanTable(pan) / 128.0;
	}
}

uint8_t NinSnesTrack::ReadPanTable(uint16_t pan)
{
	NinSnesSeq* parentSeq = (NinSnesSeq*)this->parentSeq;

	if (parentSeq->version == NINSNES_TOSE) {
		// no pan table
		return 0;
	}

	uint8_t panIndex = pan >> 8;
	uint8_t panFraction = pan & 0xff;

	uint8_t panMaxIndex = parentSeq->panTable.size() - 1;
	if (panIndex > panMaxIndex) {
		// unexpected behavior
		panIndex = panMaxIndex;
		panFraction = 0; // floor(pan)
	}

	uint8_t volumeRate = parentSeq->panTable[panIndex];

	// linear interpolation for pan fade
	uint8_t nextVolumeRate = (panIndex < panMaxIndex) ? parentSeq->panTable[panIndex + 1] : volumeRate;
	uint8_t volumeRateDelta = nextVolumeRate - volumeRate;
	volumeRate += (volumeRateDelta * panFraction) >> 8;

	return volumeRate;
}