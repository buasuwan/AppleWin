#pragma once

enum SS_CARDTYPE
{
	CT_Empty = 0,
	CT_Disk2,			// Apple Disk][
	CT_SSC,				// Apple Super Serial Card
	CT_MockingboardC,	// Soundcard
	CT_GenericPrinter,
	CT_GenericHDD,		// Hard disk
	CT_GenericClock,
	CT_MouseInterface,
	CT_Z80,
	CT_Phasor,			// Soundcard
	CT_Echo,			// Soundcard
	CT_SAM,				// Soundcard: Software Automated Mouth
	CT_80Col,			// 80 column card (1K)
	CT_Extended80Col,	// Extended 80-col card (64K)
	CT_RamWorksIII,		// RamWorksIII (up to 8MB)
	CT_Uthernet,
	CT_LanguageCard,	// Apple][ or ][+ in slot-0
	CT_LanguageCardIIe,	// Apple//e LC instance (not a card)
	CT_Saturn128K,		// Saturn 128K (but may be populated with less RAM, in multiples of 16K)
	CT_FourPlay,		// 4 port Atari 2600 style digital joystick card
	CT_SNESMAX,			// 2 port Nintendo NES/SNES controller serial interface card
};

enum SLOTS { SLOT0=0, SLOT1, SLOT2, SLOT3, SLOT4, SLOT5, SLOT6, SLOT7, NUM_SLOTS, SLOT_AUX };

class Card
{
public:
	Card(void) : m_type(CT_Empty), m_slot(SLOT0) {}
	Card(SS_CARDTYPE type, UINT slot) : m_type(type), m_slot(slot) {}
	virtual ~Card(void) {}

	virtual void Init(void) = 0;
	virtual void Reset(const bool powerCycle) = 0;
	SS_CARDTYPE QueryType(void) { return m_type; }

protected:
	UINT m_slot;

private:
	SS_CARDTYPE m_type;
};

//

class EmptyCard : public Card
{
public:
	EmptyCard(void) {}
	virtual ~EmptyCard(void) {}

	virtual void Init(void) {};
	virtual void Reset(const bool powerCycle) {};
};

//

class DummyCard : public Card	// For cards that currently can't be instantiated (ie. don't exist as a class)
{
public:
	DummyCard(SS_CARDTYPE type, UINT slot) : Card(type, slot) {}
	virtual ~DummyCard(void) {}

	virtual void Init(void) {};
	virtual void Reset(const bool powerCycle) {};
};
