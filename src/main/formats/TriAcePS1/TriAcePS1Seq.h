#pragma once
#include "VGMSeq.h"
#include "SeqTrack.h"
#include "TriAcePS1Format.h"

class TriAcePS1ScorePattern;

class TriAcePS1Seq:
    public VGMSeq {
 public:
  typedef struct _TrkInfo {
    uint16_t unknown1;
    uint16_t unknown2;
    uint16_t trkOffset;
  } TrkInfo;


  TriAcePS1Seq(RawFile *file, uint32_t offset, const std::string &name = std::string("TriAce Seq"));

  bool GetHeaderInfo() override;
  bool GetTrackPointers() override;
  void ResetVars() override;

  bool PostLoad() override;

  VGMHeader *header;
  TrkInfo TrkInfos[32];
  std::vector<TriAcePS1ScorePattern *> aScorePatterns;
  TriAcePS1ScorePattern *curScorePattern;
  std::map<uint32_t, TriAcePS1ScorePattern *> patternMap;
  uint8_t initialTempoBPM;
};

class TriAcePS1ScorePattern
    : public VGMItem {
 public:
  TriAcePS1ScorePattern(TriAcePS1Seq *parentSeq, uint32_t offset)
      : VGMItem(parentSeq, offset, 0, "Score Pattern") { }
};


class TriAcePS1Track
    : public SeqTrack {
 public:
  TriAcePS1Track(TriAcePS1Seq *parentSeq, uint32_t offset = 0, uint32_t length = 0);

  virtual void LoadTrackMainLoop(uint32_t stopOffset, int32_t stopTime);
  uint32_t ReadScorePattern(uint32_t offset);
  virtual bool IsOffsetUsed(uint32_t offset);
  virtual void AddEvent(SeqEvent *pSeqEvent);
  virtual bool ReadEvent(void);

  uint8_t impliedNoteDur;
  uint8_t impliedVelocity;
};