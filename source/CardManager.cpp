/*
AppleWin : An Apple //e emulator for Windows

Copyright (C) 1994-1996, Michael O'Brien
Copyright (C) 1999-2001, Oliver Schmidt
Copyright (C) 2002-2005, Tom Charlesworth
Copyright (C) 2006-2019, Tom Charlesworth, Michael Pohoreski, Nick Westgate

AppleWin is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

AppleWin is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with AppleWin; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

/* Description: Card Manager
 *
 * Author: Various
 *
 */

#include "StdAfx.h"

#include "CardManager.h"
#include "Core.h"
#include "Registry.h"

#include "Disk.h"
#include "FourPlay.h"
#include "Harddisk.h"
#include "MouseInterface.h"
#include "SAM.h"
#include "SerialComms.h"
#include "SNESMAX.h"

void CardManager::InsertInternal(UINT slot, SS_CARDTYPE type)
{
	RemoveInternal(slot);

	switch (type)
	{
	case CT_Empty:
		m_slot[slot] = new EmptyCard;
		break;
	case CT_Disk2:
		m_slot[slot] = new Disk2InterfaceCard(slot);
		break;
	case CT_SSC:
		_ASSERT(m_pSSC == NULL);
		if (m_pSSC) break;	// Only support one SSC
		m_slot[slot] = m_pSSC = new CSuperSerialCard(slot);
		break;
	case CT_MockingboardC:
		m_slot[slot] = new DummyCard(type, slot);
		break;
	case CT_GenericPrinter:
		m_slot[slot] = new DummyCard(type, slot);
		break;
	case CT_GenericHDD:
		m_slot[slot] = new HarddiskInterfaceCard(slot);
		break;
	case CT_GenericClock:
		m_slot[slot] = new DummyCard(type, slot);
		break;
	case CT_MouseInterface:
		_ASSERT(m_pMouseCard == NULL);
		if (m_pMouseCard) break;	// Only support one Mouse card
		m_slot[slot] = m_pMouseCard = new CMouseInterface(slot);
		break;
	case CT_Z80:
		m_slot[slot] = new DummyCard(type, slot);
		break;
	case CT_Phasor:
		m_slot[slot] = new DummyCard(type, slot);
		break;
	case CT_Echo:
		m_slot[slot] = new DummyCard(type, slot);
		break;
	case CT_SAM:
		m_slot[slot] = new SAMCard(slot);
		break;
	case CT_Uthernet:
		m_slot[slot] = new DummyCard(type, slot);
		break;
	case CT_FourPlay:
		m_slot[slot] = new FourPlayCard(slot);
		break;
	case CT_SNESMAX:
		m_slot[slot] = new SNESMAXCard(slot);
		break;

	case CT_LanguageCard:
	case CT_Saturn128K:
		{
			if (slot != 0)
			{
				_ASSERT(0);
				break;
			}
		}
		m_slot[slot] = new DummyCard(type, slot);
		break;

	case CT_LanguageCardIIe:	// not a card
	default:
		_ASSERT(0);
		break;
	}

	if (m_slot[slot] == NULL)
		Remove(slot);			// creates a new EmptyCard
}

void CardManager::Insert(UINT slot, SS_CARDTYPE type, bool updateRegistry/*=true*/)
{
	InsertInternal(slot, type);
	if (updateRegistry)
		RegSetConfigSlotNewCardType(slot, type);
}

void CardManager::RemoveInternal(UINT slot)
{
	if (m_slot[slot] && m_slot[slot]->QueryType() == CT_MouseInterface)
		m_pMouseCard = NULL;	// NB. object deleted below: delete m_slot[slot]

	if (m_slot[slot] && m_slot[slot]->QueryType() == CT_SSC)
		m_pSSC = NULL;			// NB. object deleted below: delete m_slot[slot]

	delete m_slot[slot];
	m_slot[slot] = NULL;
}

void CardManager::Remove(UINT slot, bool updateRegistry/*=true*/)
{
	Insert(slot, CT_Empty, updateRegistry);
}

void CardManager::InsertAuxInternal(SS_CARDTYPE type)
{
	RemoveAuxInternal();

	switch (type)
	{
	case CT_Empty:
		m_aux = new EmptyCard;
		break;
	case CT_80Col:
		m_aux = new DummyCard(type, SLOT_AUX);
		break;
	case CT_Extended80Col:
		m_aux = new DummyCard(type, SLOT_AUX);
		break;
	case CT_RamWorksIII:
		m_aux = new DummyCard(type, SLOT_AUX);
		break;
	default:
		_ASSERT(0);
		break;
	}

	// for consistency m_aux must never be NULL
	_ASSERT(m_aux != NULL);
	if (m_aux == NULL)
		RemoveAux();	// creates a new EmptyCard
}

void CardManager::InsertAux(SS_CARDTYPE type)
{
	InsertAuxInternal(type);
	RegSetConfigSlotNewCardType(SLOT_AUX, type);
}

void CardManager::RemoveAuxInternal()
{
	delete m_aux;
	m_aux = NULL;
}

void CardManager::RemoveAux(void)
{
	InsertAux(CT_Empty);
}
