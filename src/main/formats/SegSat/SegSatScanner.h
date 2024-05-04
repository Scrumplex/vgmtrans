/*
 * VGMTrans (c) 2002-2024
 * Licensed under the zlib license,
 * refer to the included LICENSE.txt file
 */
#pragma once
#include "Scanner.h"

class SegSatScanner : public VGMScanner {
 public:
  SegSatScanner();
  virtual ~SegSatScanner();

  virtual void Scan(RawFile *file, void *info = 0);
};