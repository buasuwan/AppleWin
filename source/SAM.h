#pragma once

#include "Card.h"

class SAMCard : public Card
{
public:
	SAMCard(UINT slot) :
		Card(CT_SAM, slot)
	{
	}
	virtual ~SAMCard(void) {}

	virtual void Init(void) {};
	virtual void Reset(const bool powerCycle) {};

	void InitializeIO(LPBYTE pCxRomPeripheral);

	static BYTE __stdcall IOWrite(WORD pc, WORD addr, BYTE bWrite, BYTE value, ULONG nExecutedCycles);

	static std::string GetSnapshotCardName(void);
	void SaveSnapshot(class YamlSaveHelper& yamlSaveHelper);
	bool LoadSnapshot(class YamlLoadHelper& yamlLoadHelper, UINT slot, UINT version);

private:
	// no state
};
