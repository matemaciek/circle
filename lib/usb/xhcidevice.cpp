//
// xhcidevice.cpp
//
// Circle - A C++ bare metal environment for Raspberry Pi
// Copyright (C) 2019  R. Stange <rsta2@o2online.de>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#include <circle/usb/xhcidevice.h>
#include <circle/bcm2711.h>
#include <circle/memio.h>
#include <circle/logger.h>
#include <circle/memory.h>
#include <circle/util.h>
#include <assert.h>

static const char From[] = "xhci";

CXHCIDevice::CXHCIDevice (CInterruptSystem *pInterruptSystem, CTimer *pTimer)
:	m_PCIeHostBridge (pInterruptSystem),
	m_nSharedMemStart (CMemorySystem::GetCoherentPage (COHERENT_SLOT_XHCI_START)),
	m_nSharedMemEnd (CMemorySystem::GetCoherentPage (COHERENT_SLOT_XHCI_END) + PAGE_SIZE - 1),
	m_pMMIO (0),
	m_pSlotManager (0),
	m_pEventManager (0),
	m_pCommandManager (0),
	m_pScratchpadBuffers (0),
	m_pScratchpadBufferArray (0),
	m_pRootHub (0),
	m_bShutdown (FALSE)
{
	assert (m_nSharedMemStart != 0);
	assert (m_nSharedMemEnd != 0);
}

CXHCIDevice::~CXHCIDevice (void)
{
	m_bShutdown = TRUE;

	if (m_pMMIO != 0)
	{
		m_pMMIO->op_write32 (XHCI_REG_OP_USBCMD,   m_pMMIO->op_read32 (XHCI_REG_OP_USBCMD)
							& ~XHCI_REG_OP_USBCMD_RUN_STOP);

		HWReset ();
	}

	m_pScratchpadBufferArray = 0;
	m_pScratchpadBuffers = 0;

	delete m_pCommandManager;
	m_pCommandManager = 0;

	delete m_pEventManager;
	m_pEventManager = 0;

	delete m_pSlotManager;
	m_pSlotManager = 0;

	delete m_pMMIO;
	m_pMMIO = 0;

	m_nSharedMemStart = 0;
	m_nSharedMemEnd = 0;
}

boolean CXHCIDevice::Initialize (void)
{
	// init class-specific allocators in USB library
	INIT_PROTECTED_CLASS_ALLOCATOR (CUSBRequest, XHCI_CONFIG_MAX_REQUESTS, IRQ_LEVEL);

	// PCIe init
	if (!m_PCIeHostBridge.Initialize ())
	{
		CLogger::Get ()->Write (From, LogError, "Cannot initialize PCIe host bridge");

		return FALSE;
	}

	if (!m_PCIeHostBridge.ConnectMSI (InterruptStub, this))
	{
		CLogger::Get ()->Write (From, LogError, "Cannot connect MSI");

		return FALSE;
	}

	if (!m_PCIeHostBridge.EnableDevice (XHCI_PCI_CLASS_CODE, XHCI_PCIE_SLOT, XHCI_PCIE_FUNC))
	{
		CLogger::Get ()->Write (From, LogError, "Cannot enable xHCI device");

		return FALSE;
	}

	// version check
	u16 usVersion = read16 (ARM_XHCI_BASE + XHCI_REG_CAP_HCIVERSION);
	if (usVersion != XHCI_SUPPORTED_VERSION)
	{
		CLogger::Get ()->Write (From, LogError, "Unsupported xHCI version (%X)",
					(unsigned) usVersion);

		return FALSE;
	}

	// init MMIO space
	m_pMMIO = new CXHCIMMIOSpace (ARM_XHCI_BASE);
	assert (m_pMMIO != 0);

	// get some capabilities
#ifndef NDEBUG
	unsigned nMaxSlots =   m_pMMIO->cap_read32 (XHCI_REG_CAP_HCSPARAMS1)
			     & XHCI_REG_CAP_HCSPARAMS1_MAX_SLOTS__MASK;
	assert (XHCI_CONFIG_MAX_SLOTS <= nMaxSlots);
#endif

	unsigned nMaxPorts =      (m_pMMIO->cap_read32 (XHCI_REG_CAP_HCSPARAMS1)
			        & XHCI_REG_CAP_HCSPARAMS1_MAX_PORTS__MASK)
			     >> XHCI_REG_CAP_HCSPARAMS1_MAX_PORTS__SHIFT;
	assert (XHCI_CONFIG_MAX_PORTS >= nMaxPorts);

	unsigned nMaxScratchpadBufs =      (m_pMMIO->cap_read32 (XHCI_REG_CAP_HCSPARAMS2)
				         & XHCI_REG_CAP_HCSPARAMS2_MAX_SCRATCHPAD_BUFS__MASK)
				      >> XHCI_REG_CAP_HCSPARAMS2_MAX_SCRATCHPAD_BUFS__SHIFT;

#ifdef XHCI_DEBUG
	CLogger::Get ()->Write (From, LogDebug, "%u slots, %u ports, %u scratchpad bufs",
				nMaxSlots, nMaxPorts, nMaxScratchpadBufs);
#endif

	// HC initialization
	if (!HWReset ())
	{
		CLogger::Get ()->Write (From, LogError, "HW reset failed");

		return FALSE;
	}

	// our used page size must be supported
	assert (XHCI_PAGE_SIZE == 4096);
	assert (m_pMMIO->op_read32 (XHCI_REG_OP_PAGESIZE) & XHCI_REG_OP_PAGESIZE_4K);

	// create manager objects
	m_pSlotManager = new CXHCISlotManager (this);
	assert (m_pSlotManager != 0);

	m_pEventManager = new CXHCIEventManager (this);
	assert (m_pEventManager != 0);

	m_pCommandManager = new CXHCICommandManager (this);
	assert (m_pCommandManager != 0);

	if (   !m_pSlotManager->IsValid ()
	    || !m_pEventManager->IsValid ()
	    || !m_pCommandManager->IsValid ())
	{
		CLogger::Get ()->Write (From, LogError,
					"Cannot init slot, event or command manager");

		return FALSE;
	}

	// init scratchpad
	assert (nMaxScratchpadBufs > 0);
	m_pScratchpadBuffers = AllocateSharedMem (XHCI_PAGE_SIZE * nMaxScratchpadBufs,
						  XHCI_PAGE_SIZE);
	m_pScratchpadBufferArray = (u64 *) AllocateSharedMem (sizeof (u64) * nMaxScratchpadBufs);
	if (   m_pScratchpadBuffers == 0
	    || m_pScratchpadBufferArray == 0)
	{
		return FALSE;	// logger message was generated inside AllocateSharedMem()
	}

	for (unsigned i = 0; i < nMaxScratchpadBufs; i++)
	{
		m_pScratchpadBufferArray[i] = XHCI_TO_DMA (m_pScratchpadBuffers) + XHCI_PAGE_SIZE*i;
	}

	m_pSlotManager->AssignScratchpadBufferArray (m_pScratchpadBufferArray);

	// create root hub
	m_pRootHub = new CXHCIRootHub (nMaxPorts, this);
	assert (m_pRootHub != 0);

	// start controller
	m_pMMIO->op_write32 (XHCI_REG_OP_USBCMD,   m_pMMIO->op_read32 (XHCI_REG_OP_USBCMD)
						 | XHCI_REG_OP_USBCMD_INTE);

	m_pMMIO->op_write32 (XHCI_REG_OP_USBCMD,   m_pMMIO->op_read32 (XHCI_REG_OP_USBCMD)
						 | XHCI_REG_OP_USBCMD_RUN_STOP);

	// init root hub
	if (!m_pRootHub->Initialize ())
	{
		CLogger::Get ()->Write (From, LogError, "Cannot init root hub");

		return FALSE;
	}

#if !defined (NDEBUG) && defined (XHCI_DEBUG2)
	DumpStatus ();
#endif

	return TRUE;
}

void CXHCIDevice::ReScanDevices (void)	// TODO
{
}

boolean CXHCIDevice::SubmitBlockingRequest (CUSBRequest *pURB, unsigned nTimeoutMs)
{
	if (m_bShutdown)
	{
		return FALSE;
	}

	assert (pURB != 0);
	CXHCIEndpoint *pEndpoint = pURB->GetEndpoint ()->GetXHCIEndpoint ();
	assert (pEndpoint != 0);

	return pEndpoint->Transfer (pURB, nTimeoutMs);
}

boolean CXHCIDevice::SubmitAsyncRequest (CUSBRequest *pURB, unsigned nTimeoutMs)
{
	if (m_bShutdown)
	{
		return FALSE;
	}

	assert (pURB != 0);
	CXHCIEndpoint *pEndpoint = pURB->GetEndpoint ()->GetXHCIEndpoint ();
	assert (pEndpoint != 0);

	return pEndpoint->TransferAsync (pURB, nTimeoutMs);
}

CXHCIMMIOSpace *CXHCIDevice::GetMMIOSpace (void)
{
	assert (m_pMMIO != 0);
	return m_pMMIO;
}

CXHCISlotManager *CXHCIDevice::GetSlotManager (void)
{
	assert (m_pSlotManager != 0);
	return m_pSlotManager;
}

CXHCICommandManager *CXHCIDevice::GetCommandManager (void)
{
	assert (m_pCommandManager != 0);
	return m_pCommandManager;
}

CXHCIRootHub *CXHCIDevice::GetRootHub (void)
{
	assert (m_pRootHub != 0);
	return m_pRootHub;
}

void *CXHCIDevice::AllocateSharedMem (size_t nSize, size_t nAlign, size_t nBoundary)
{
	assert (nSize > 0);
	assert (nAlign != 0);
	assert (nAlign <= nBoundary);
	assert (m_nSharedMemStart != 0);
	assert (m_nSharedMemEnd != 0);

	size_t nAlignMask = nAlign - 1;
	if (m_nSharedMemStart & nAlignMask)
	{
		m_nSharedMemStart += nAlignMask;
		m_nSharedMemStart &= ~nAlignMask;
	}

	size_t nBoundaryMask = nBoundary - 1;
	if (   (m_nSharedMemStart & ~nBoundaryMask)
	    != ((m_nSharedMemStart + nSize-1) & ~nBoundaryMask))
	{
		m_nSharedMemStart += nBoundaryMask;
		m_nSharedMemStart &= ~nBoundaryMask;
	}

	void *pResult = (void *) m_nSharedMemStart;

	m_nSharedMemStart += nSize;
	if (m_nSharedMemStart <= m_nSharedMemEnd)
	{
		memset (pResult, 0, nSize);
	}
	else
	{
		CLogger::Get ()->Write (From, LogError, "Shared memory space exhausted");

		pResult = 0;
	}

	return pResult;
}

void CXHCIDevice::FreeSharedMem (void *pBlock)	// TODO
{
#ifdef XHCI_DEBUG
	CLogger::Get ()->Write (From, LogWarning,
				"Trying to free shared memory at 0x%lX", (uintptr) pBlock);
#endif
}

void CXHCIDevice::InterruptHandler (unsigned nVector)
{
#ifdef XHCI_DEBUG2
	CLogger::Get ()->Write (From, LogDebug, "MSI%u", nVector);
#endif
	assert (nVector == 0);

	// acknowledge interrupt
	u32 nStatus = m_pMMIO->op_read32 (XHCI_REG_OP_USBSTS);
	m_pMMIO->op_write32 (XHCI_REG_OP_USBSTS, nStatus | XHCI_REG_OP_USBSTS_EINT);

	if (nStatus & XHCI_REG_OP_USBSTS_HCH)
	{
		CLogger::Get ()->Write (From, LogError, "HC halted");

		return;
	}

	if (m_bShutdown)
	{
		return;
	}

	assert (m_pEventManager != 0);
	while (m_pEventManager->HandleEvents ())
	{
		// just loop
	}
}

void CXHCIDevice::InterruptStub (unsigned nVector, void *pParam)
{
	CXHCIDevice *pThis = (CXHCIDevice *) pParam;
	assert (pThis != 0);

	pThis->InterruptHandler (nVector);
}

boolean CXHCIDevice::HWReset (void)
{
	if (   !m_pMMIO->op_wait32 (XHCI_REG_OP_USBSTS, XHCI_REG_OP_USBSTS_CNR, 0, 100000)
	    || !m_pMMIO->op_wait32 (XHCI_REG_OP_USBSTS, XHCI_REG_OP_USBSTS_HCH,
				    XHCI_REG_OP_USBSTS_HCH, 100000))
	{
		return FALSE;
	}

	m_pMMIO->op_write32 (XHCI_REG_OP_USBCMD, m_pMMIO->op_read32 (XHCI_REG_OP_USBCMD)
						 | XHCI_REG_OP_USBCMD_HCRST);

	if (!m_pMMIO->op_wait32 (XHCI_REG_OP_USBCMD, XHCI_REG_OP_USBCMD_HCRST, 0, 20000))
	{
		return FALSE;
	}

	assert (!(m_pMMIO->op_read32 (XHCI_REG_OP_USBSTS) & XHCI_REG_OP_USBSTS_CNR));

	return TRUE;
}

#ifndef NDEBUG

void CXHCIDevice::DumpStatus (void)
{
	m_pRootHub->DumpStatus ();
	m_pEventManager->DumpStatus ();
	m_pCommandManager->DumpStatus ();
	m_pSlotManager->DumpStatus ();
	m_pMMIO->DumpStatus ();

	m_PCIeHostBridge.DumpStatus (XHCI_PCIE_SLOT, XHCI_PCIE_FUNC);

	CLogger::Get ()->Write (From, LogDebug, "%u KB shared memory free",
				  m_nSharedMemEnd > m_nSharedMemStart
				? (m_nSharedMemEnd - m_nSharedMemStart) / 1024 : 0);
}

#endif