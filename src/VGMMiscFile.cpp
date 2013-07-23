#include "stdafx.h"
#include "VGMMiscFile.h"
#include "Root.h"

// ***********
// VGMMiscFile
// ***********

VGMMiscFile::VGMMiscFile(const string& format, RawFile* file, ULONG offset, ULONG length, wstring name)
: VGMFile(FILETYPE_MISC, format, file, offset, length, name)
{

}

bool VGMMiscFile::LoadMain()
{
	return true;
}

bool VGMMiscFile::Load()
{
	if (!LoadMain())
		return false;
	if (unLength == 0)
		return false;

	LoadLocalData();
	UseLocalData();
	pRoot->AddVGMFile(this);
	return true;
}
