#include "pch.h"
#include "CManagedHeap.h"


//////////////////////////////////////////////////////////////////////////
// 
//////////////////////////////////////////////////////////////////////////
CManagedHeap::CManagedHeap() :
	m_pMemory(nullptr),
	m_ELastHeapError(EHeapError_Ok),
	m_bSelfAllocatedMemory(false)
{
}


//////////////////////////////////////////////////////////////////////////
// 
//////////////////////////////////////////////////////////////////////////
CManagedHeap::~CManagedHeap()
{
	if (m_pMemory != nullptr)
	{
		//DID NOT CALL SHUTDOWN FIRST
		_ASSERT(false);
	}
}


//////////////////////////////////////////////////////////////////////////
// Sets up the heap by requesting memory itself from the OS
//////////////////////////////////////////////////////////////////////////
void CManagedHeap::Initialise(u32 uMemorySizeInBytes)
{
	//Request memory from the OS to manage ourselves
	u8* pRawMemory = (u8*)malloc(uMemorySizeInBytes);

	if (pRawMemory) //Malloc was sucessful (did not return nulltr)
	{
		m_bSelfAllocatedMemory = true;
		Initialise(pRawMemory, uMemorySizeInBytes);
		if (m_ELastHeapError != EHeapError_Ok) //Initialistion method failed somehow
		{
			free(pRawMemory); //Free the memory back to the OS
		}
	}
	else  //Malloc failed sucessful
	{
		m_ELastHeapError = EHeapState_Init_UnableToAquireMemory;
	}
}

//////////////////////////////////////////////////////////////////////////
// Sets up the heap using memory already allocated to this program
//////////////////////////////////////////////////////////////////////////
void CManagedHeap::Initialise(u8* pRawMemory, u32 uMemorySizeInBytes)
{
	// Early break out statements, done seperately as they return their own error codes

	if (m_pMemory) // If we have any memory set already, return early 
	{
		m_ELastHeapError = EHeapState_Init_AlreadyInitialised;
		return;
	}

	if (!pRawMemory) //Passed nullptr, return early
	{
		m_ELastHeapError = EHeapState_Init_UnableToAquireMemory;
		return;
	}

	if (!IsAligned(pRawMemory))// If memory isn't correctly alligned
	{
		m_ELastHeapError = EHeapState_Init_BadAlign;
		return;
	}

	//At this stage we have statisfied all the condidtions for setting up the heap
	m_pMemory = pRawMemory;
	m_uMemorySize = uMemorySizeInBytes;

	m_uFreeSpace = uMemorySizeInBytes;
	m_uActualFreeSpace = uMemorySizeInBytes;

	m_uNumAllocations = 0;

	m_pBlock = EncapsulateMemoryBlock(m_pMemory, m_uMemorySize);

	m_ELastHeapError = EHeapError_Ok;

}

//////////////////////////////////////////////////////////////////////////
// Explicit shutdown - releases memory if it was claimed by this class,call before destructor
//////////////////////////////////////////////////////////////////////////
void CManagedHeap::Shutdown()
{
	//Only free the memory if we aquired it ourself
	if (m_bSelfAllocatedMemory)
	{
		free(m_pMemory);
	}

	m_pMemory = nullptr;
}




//////////////////////////////////////////////////////////////////////////
// Allocates the specified size of memory, with the specified alignment
// and returns a pointer to it.
//////////////////////////////////////////////////////////////////////////
void* CManagedHeap::Allocate(u32 uNumBytes, u32 uAlignment)
{
	m_ELastHeapError = EHeapError_Ok;

	//Early break outs
	if (!m_pMemory) //Heap had not been initialised
	{
		m_ELastHeapError = EHeapState_Init_NotInitialised;
		return nullptr;
	}
	if (!IsPowerOfTwo(uAlignment) || uAlignment < _PLATFORM_MIN_ALIGN) //Bad alignment values
	{
		m_ELastHeapError = EHeapState_Alloc_BadAlign;
		return nullptr;
	}

	if (uNumBytes == 0)
	{
		m_ELastHeapError = EHeapState_Alloc_ZeroSizeAlloc;
		return nullptr;
	}

	//Always allocate memory in multiples of 4 bytes (helps keep blocks regular sized and reduces allignment padding)
	if (uNumBytes % 4 != 0)
	{
		uNumBytes += 4 - (uNumBytes % 4);
	}


	SBlockHeader* pBlockToAllocateTo = FindFreeBlock(uNumBytes, uAlignment);
	if (!pBlockToAllocateTo) //Could not find a free block for this size
	{
		m_ELastHeapError = EHeapState_Alloc_NoLargeEnoughBlocks;
		return nullptr;
	}

	AdjustBlockPositionForPadding(uAlignment, pBlockToAllocateTo);

	ManageFreeSpacePostAllocation(pBlockToAllocateTo, uNumBytes);

	pBlockToAllocateTo->m_bIsFreeBlock = false;
	pBlockToAllocateTo->m_uBlockSize = uNumBytes;
	m_uActualFreeSpace -= uNumBytes;
	m_uFreeSpace -= uNumBytes;
	WriteFooter(pBlockToAllocateTo);

	u8 *returnptr = (u8*)pBlockToAllocateTo;
	returnptr += sizeof(SBlockHeader);

	m_uNumAllocations++;
	return returnptr;

}

//////////////////////////////////////////////////////////////////////////
// deallocates the memory pointed to by pMemory and returns it to the 
// free memory stored in the heap.
//////////////////////////////////////////////////////////////////////////
void CManagedHeap::Deallocate(void* pMemory)
{
	m_ELastHeapError = EHeapError_Ok;
	//Nullptr, return early
	if (!pMemory)
	{
		m_ELastHeapError = EHeapState_Dealloc_Nullptr;
		return;

	}

	u8* pMemoryBlock = (u8*)pMemory;
	pMemoryBlock -= sizeof(SBlockHeader); //Find the header for this block

	SBlockHeader* pHeader = (SBlockHeader*)pMemoryBlock;

	//Block was already free, return early
	if (pHeader->m_bIsFreeBlock)
	{
		m_ELastHeapError = EHeapState_Dealloc_AlreadyDeallocated;
		return;
	}

	//Mark the block as free, then update counters
	pHeader->m_bIsFreeBlock = true;
	m_uNumAllocations--;
	m_uFreeSpace += pHeader->m_uBlockSize;
	m_uActualFreeSpace += pHeader->m_uBlockSize;

	/////////////////////////////////////////////////////
	// Check block integrity
	//If we want to check for underrun, compare the values of our right padding field (the one most likely to get overritten)
	//To the next blocks Left value. These should both be identical, and if not, it is likely our value has been corrupted
	if (pHeader->m_RightPadding != pHeader->m_pSMemBlockNext->m_LeftPadding)
	{
		m_ELastHeapError = EHeapState_Dealloc_OverwriteUnderrun;
	}

	//If the pointer in the footer to the header does not match where the header should be, it is likely to have been overritten and therefore corrupted
	if (GetFooter(pHeader)->m_pMatchingHeader != pHeader)
	{
		m_ELastHeapError = EHeapState_Dealloc_OverwriteOverrun;
	}
	/////////////////////////////////////////////////////

	//Try to coalese with nearby freeblocks and padding

	u8* pStartOfBlockToMerge = (u8*)pHeader;
	u8* pEndOfBlockToMerge = (u8*)GetFooter(pHeader) + sizeof(SFooterBlock);
	MergeWithNearbyBlocks(pStartOfBlockToMerge, pEndOfBlockToMerge);

}



//////////////////////////////////////////////////////////////////////////
//Returns the freespace available, accounting for overheads
//////////////////////////////////////////////////////////////////////////
u32 CManagedHeap::GetFreeMemory()
{
	if (m_uNumAllocations != 0)
	{
		return m_uActualFreeSpace;
	}
	else
	{
		return m_uFreeSpace;
	}
}


//////////////////////////////////////////////////////////////////////////
// Calcuates the offset to add to a pointer to align it to the alignment passed
// Must be a power of two
//////////////////////////////////////////////////////////////////////////
u32 CManagedHeap::CalculateAlignmentDelta(void* pPointerToAlign, u32 uAlignment)
{
	u32 uAlignAdd = uAlignment - 1;
	u32 uAlignMask = ~uAlignAdd;
	u32 uAddressToAlign = ((u32)pPointerToAlign);
	u32 uAligned = uAddressToAlign + uAlignAdd;
	uAligned &= uAlignMask;
	return(uAligned - uAddressToAlign);
}

//////////////////////////////////////////////////////////////////////////
// Prints the current state of the managed memory in a friendly format
//////////////////////////////////////////////////////////////////////////
void CManagedHeap::Print()
{
	HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);

	int bytesFree = 0;
	int bytesAllocatted = 0;
	int bytesInOverheads = 0;
	int bytesInPadding = 0;
	int numberOfBlocks = 0;
	int largestFreeBlock = 0;

	SBlockHeader* block = m_pBlock;
	do
	{
		numberOfBlocks++;
		bytesInOverheads += sizeof(SBlockHeader) + sizeof(SFooterBlock);

		SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), 14);

		if (block->m_LeftPadding > 0)
		{
			bytesInPadding += block->m_LeftPadding;
			for (int i = 0; i < block->m_LeftPadding; i += 4) //Assumes padding was in 4bytes
			{
				std::cout << "PADD" << "  ";
			}
		}

		SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), 10);


		if (block->m_bIsFreeBlock)
		{
			std::cout << "FREE" << "  ";
		}
		else
		{
			std::cout << "DATA" << "  ";
		}

		if (block->m_pSMemBlockNext != nullptr)
		{
			std::cout << "NBLK" << "  ";
		}
		else
		{
			std::cout << "NULL" << "  ";
		}

		std::cout << std::setfill('0') << std::setw(4) << block->m_uBlockSize;

		std::cout << "  ";
		std::cout << "LPAD" << "  ";
		std::cout << "RPAD" << "  ";
		u8 bytecolour;

		if (block->m_bIsFreeBlock)
		{
			bytecolour = 11;
			bytesFree += block->m_uBlockSize;
			if (block->m_uBlockSize > largestFreeBlock)
			{
				largestFreeBlock = block->m_uBlockSize;
			}
		}
		else
		{
			bytecolour = 176;
			bytesAllocatted += block->m_uBlockSize;
		}
		u8 *ptr = (u8*)block;
		ptr += sizeof(SBlockHeader);
		for (int i = 0; i < block->m_uBlockSize; i++) //Prints the data in 4 byte blocks
		{
			SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), bytecolour);
			std::cout << ptr[i];

			if ((i + 1) % 4 == 0)
			{
				SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), 7);
				std::cout << "  ";
			}

		}
		ptr += block->m_uBlockSize;
		SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), 12);
		SFooterBlock* foot = (SFooterBlock*)ptr;
		int size = foot->m_uSizeOfBlock;
		if (foot->m_pMatchingHeader == block)
		{
			std::cout << "HEAD" << "  ";
		}
		else
		{
			std::cout << "!BAD" << "  ";
		}
		std::cout << std::setfill('0') << std::setw(4) << size;
		std::cout << "  ";

		if (block->m_pSMemBlockNext != nullptr)
		{
			int space = block->m_RightPadding - block->m_pSMemBlockNext->m_LeftPadding;
			if (0 != space)
			{
				SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), 64);
				for (int i = 0; i < space; i += 4)
				{
					std::cout << "ERRR" << "  ";
				}
			}
		}
		else
		{
			SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), 14);
			if (block->m_RightPadding > 0)
			{
				bytesInPadding += block->m_RightPadding;
				for (int i = 0; i < block->m_RightPadding; i += 4)
				{
					std::cout << "PADD" << "  ";
				}
			}
		}

		if (block->m_pSMemBlockNext != nullptr)
		{
			block = block->m_pSMemBlockNext;
		}
		else
		{
			break;
		}

	} while (true);
	SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), 7);

	std::cout << std::endl;
	std::cout << "Free bytes " << bytesFree << " / " << m_uMemorySize << std::endl;;
	std::cout << "Allocated bytes " << bytesAllocatted << " / " << m_uMemorySize << std::endl;
	std::cout << "Overhead btyes " << bytesInOverheads << " / " << m_uMemorySize << std::endl;
	std::cout << "Padding bytes " << bytesInPadding << " / " << m_uMemorySize << std::endl;
	std::cout << "Number of allocation blocks " << numberOfBlocks << std::endl;
	std::cout << "Largest free block " << largestFreeBlock << " Bytes" << std::endl;

}

//////////////////////////////////////////////////////////////////////////
// Prints the current state of the managed memory directly
//////////////////////////////////////////////////////////////////////////
void CManagedHeap::PrintDUMP()
{
	for (int i = 0; i < m_uMemorySize; i++)
	{
		std::cout << m_pMemory[i];
	}
	std::cout << std::endl;
}

//////////////////////////////////////////////////////////////////////////
// Validates if a pointer is alligned to min platform alignment
//////////////////////////////////////////////////////////////////////////
bool CManagedHeap::IsAligned(u8 * pRawMemory)
{
	// Modified from: https://stackoverflow.com/questions/42093360/how-to-check-if-a-pointer-points-to-a-properly-aligned-memory-location
	uintptr_t iptr = reinterpret_cast<std::uintptr_t>(pRawMemory);
	return !(iptr % _PLATFORM_MIN_ALIGN);
}

//////////////////////////////////////////////////////////////////////////
// Writes the footer for a given header block
// Returns the a pointer to the footer
/////////////////////////////////////////////////////////////////////////
CManagedHeap::SFooterBlock* CManagedHeap::WriteFooter(SBlockHeader* headerBlock)
{
	SFooterBlock* pFooterLocation = GetFooter(headerBlock);
	pFooterLocation = new (pFooterLocation) SFooterBlock;
	pFooterLocation->m_uSizeOfBlock = headerBlock->m_uBlockSize;
	pFooterLocation->m_pMatchingHeader = headerBlock;
	return pFooterLocation;
}

//////////////////////////////////////////////////////////////////////////
// Gets the position of a footer for a given header
/////////////////////////////////////////////////////////////////////////
CManagedHeap::SFooterBlock* CManagedHeap::GetFooter(SBlockHeader* headerBlock)
{
	u8 *pData = (u8*)headerBlock;
	pData += sizeof(SBlockHeader); // Move to end of Header

	pData += headerBlock->m_uBlockSize; //Move past the data
	return (SFooterBlock*)pData;
}

//////////////////////////////////////////////////////////////////////////
// Gets the position of the previous header, given a header
// Will return nullptr if this is the first header
/////////////////////////////////////////////////////////////////////////
CManagedHeap::SBlockHeader * CManagedHeap::GetPreviousHeader(SBlockHeader* headerBlock)
{
	if (headerBlock != m_pBlock) // Not first pointer
	{
		u8 *pData = (u8*)headerBlock;
		pData -= headerBlock->m_LeftPadding;
		pData -= sizeof(SFooterBlock);
		SFooterBlock* footer = (SFooterBlock*)pData;
		return footer->m_pMatchingHeader;
	}
	else
	{
		return nullptr;
	}
}

//////////////////////////////////////////////////////////////////////////
// Sets up a header footer pair arround a block of a given size
// Pointer should be pointing to where the header should be placed
// Returns a pointer to the header created
/////////////////////////////////////////////////////////////////////////
CManagedHeap::SBlockHeader* CManagedHeap::EncapsulateMemoryBlock(u8 * pRawMemory, u32 uSizeOfBlock)
{
	SBlockHeader* pHeader = new (pRawMemory) SBlockHeader;

	pHeader->m_bIsFreeBlock = true;
	pHeader->m_pSMemBlockNext = nullptr;
	pHeader->m_uBlockSize = uSizeOfBlock - (sizeof(SBlockHeader) + sizeof(SFooterBlock));
	pHeader->m_LeftPadding = 0;
	pHeader->m_RightPadding = 0;
	WriteFooter(pHeader); //Writes corresponding footers

	m_uActualFreeSpace -= (sizeof(SBlockHeader) + sizeof(SFooterBlock)); //Remove size of overheads from actual free space

	return pHeader;
}

//////////////////////////////////////////////////////////////////////////
// Itterate through the blocks, checking for a block which is free,matches our allignment, and could be large enough to allocate to
// The block may not currently be of the corect size, but reclaiming padding will meet the requirements
// Returns pointer to first block which satisfies the criteria, nullptr otherwise
/////////////////////////////////////////////////////////////////////////
CManagedHeap::SBlockHeader* CManagedHeap::FindFreeBlock(u32 uSizeOfBlockToFind, u32 uAlignment)
{
	//Find Free Block
	bool found = false;
	SBlockHeader* pBlockToCheck = m_pBlock;		//Get the first block
	do
	{
		if (IsBlockViable(pBlockToCheck, uSizeOfBlockToFind, uAlignment)) // Size found good enough
		{
			return pBlockToCheck;
		}
		pBlockToCheck = pBlockToCheck->m_pSMemBlockNext; // move to next block
	} while (pBlockToCheck); //While not nullptr, would have already returned if found block

	m_ELastHeapError = EHeapState_Alloc_NoLargeEnoughBlocks;
	return nullptr;
}

//////////////////////////////////////////////////////////////////////////
// Validates if a given unsigned interger is a power of two
/////////////////////////////////////////////////////////////////////////
bool CManagedHeap::IsPowerOfTwo(u32 uNumberToTest)
{
	bool isNonZero = (uNumberToTest != 0);
	bool isPow2 = (((uNumberToTest) & (uNumberToTest - 1)) == 0);
	return(isNonZero && isPow2); // Uses binary AND
}

//////////////////////////////////////////////////////////////////////////
// Determines if a block is viable, given the block to check, the size of the allocation and the alignment required
// This function takes into account reclaiming padding for this block
/////////////////////////////////////////////////////////////////////////
bool CManagedHeap::IsBlockViable(SBlockHeader* pBlockToCheck, u32 uSizeOfBlockToFind, u32 uAlignment)
{
	if (!pBlockToCheck->m_bIsFreeBlock)
	{
		return false;
	}

	u8* pMemoryAllocationStart = (u8*)pBlockToCheck - pBlockToCheck->m_LeftPadding; //See if reclaiming padding will help
	pMemoryAllocationStart += sizeof(SBlockHeader);
	u32 paddingRequired = CalculateAlignmentDelta(pMemoryAllocationStart, uAlignment);// Calculate how much padding is needed
	u32 uTotalSizeRequired = paddingRequired + uSizeOfBlockToFind;

	if (uTotalSizeRequired > pBlockToCheck->m_uBlockSize + pBlockToCheck->m_RightPadding)// Too big for this block including padding reclaimation
	{
		return false;
	}

	return true;
}

//////////////////////////////////////////////////////////////////////////
// Moves the position of the header to correct allignment, reclaiming or adding padding if required
// WILL CHANGE THE ADDRESS OF THE POINTER, therefore the varible itself is passed by reference
/////////////////////////////////////////////////////////////////////////
void CManagedHeap::AdjustBlockPositionForPadding(u32 uAlignment, SBlockHeader*& pBlockToAllocateTo)
{
	SBlockHeader* pPreviousBlock = GetPreviousHeader(pBlockToAllocateTo);

	if (pPreviousBlock) //If there is a previous block, and there is padding we can reclaim
	{

		u8* pFooterPos = (u8*)GetFooter(pPreviousBlock); //Gets the start of the footter

		pFooterPos += sizeof(SFooterBlock);			//Moves pointer to the end of the footer


		u8* pMemoryStart = pFooterPos += sizeof(SBlockHeader); // Start of our allocation, end of the previous blocks footer, plus the size of our header


		u32 uPadding = CalculateAlignmentDelta(pMemoryStart, uAlignment);

		pMemoryStart += uPadding; //Offset our memory allocation with the padding

		u8* pHeaderStart = pMemoryStart - sizeof(SBlockHeader); //Find position for our header

		SBlockHeader* pNextBlock = pBlockToAllocateTo->m_pSMemBlockNext; //Store the pointer for the next block so we dont loose in destroying the header

		pBlockToAllocateTo->~SBlockHeader(); //Destroy old header

		pBlockToAllocateTo = new (pHeaderStart) SBlockHeader(); //Placement new - using the position of the header calculated previously

		pBlockToAllocateTo->m_pSMemBlockNext = pNextBlock;
		pBlockToAllocateTo->m_LeftPadding = uPadding;
		pPreviousBlock->m_pSMemBlockNext = pBlockToAllocateTo; // Update pointer for previous block

		pPreviousBlock->m_RightPadding = pBlockToAllocateTo->m_LeftPadding; //Inform previous block of new padding size
	}
	else
	{
		//Special case, our block was the first block

		u8* pMemoryStart = m_pMemory + sizeof(SBlockHeader); //Use the base pointer as we are the first block

		u32 uPadding = CalculateAlignmentDelta(pMemoryStart, uAlignment);

		pMemoryStart += uPadding;

		u8* pHeaderStart = pMemoryStart - sizeof(SBlockHeader); //Calculated position of new header

		SBlockHeader* pNextBlock = pBlockToAllocateTo->m_pSMemBlockNext;//Store the pointer for the next block so we dont loose in destroying the header

		pBlockToAllocateTo->~SBlockHeader(); //Destroy old header

		pBlockToAllocateTo = new (pHeaderStart) SBlockHeader();

		pBlockToAllocateTo->m_pSMemBlockNext = pNextBlock;
		pBlockToAllocateTo->m_LeftPadding = uPadding;

		m_pBlock = pBlockToAllocateTo; //Update class pointer to the first block position
	}

}

//////////////////////////////////////////////////////////////////////////
// Evaluates the free space after our allocation, and if the space is large enough,
// encapsulating it with a header and footer. If not, the data is marked as padding to be reclaimed later
/////////////////////////////////////////////////////////////////////////
void CManagedHeap::ManageFreeSpacePostAllocation(SBlockHeader* pBlockToAllocateTo, u32 uNumBytes)
{
	//Calculate position to place new block for freespace
//Moves to the end of our new block and points to the first bytes of memory
	u8 *pNewBlockPointer = (u8*)pBlockToAllocateTo;
	pNewBlockPointer += sizeof(SBlockHeader);
	pNewBlockPointer += uNumBytes;
	pNewBlockPointer += sizeof(SFooterBlock);

	u32 sizeOfFreespace;

	//Check if there is space to place a new block, else put padding
	if (pBlockToAllocateTo->m_pSMemBlockNext) //If this is not the end of the memory block
	{
		sizeOfFreespace = (u8*)pBlockToAllocateTo->m_pSMemBlockNext - pNewBlockPointer;
	}
	else
	{
		sizeOfFreespace = (m_pMemory + (sizeof(u8)*m_uMemorySize)) - pNewBlockPointer;
	}


	if (sizeOfFreespace < sizeof(SBlockHeader) + sizeof(SFooterBlock) + _PLATFORM_MIN_ALIGN) //If the freespace is smaller than the overheads
	{
		pBlockToAllocateTo->m_RightPadding = sizeOfFreespace; //The freespace is marked as padding
		pBlockToAllocateTo->m_pSMemBlockNext->m_LeftPadding = sizeOfFreespace;//The freespace is marked as padding
	}
	else //Sufficient space for a block
	{
		SBlockHeader* pNewBlock = EncapsulateMemoryBlock(pNewBlockPointer, sizeOfFreespace);

		pNewBlock->m_pSMemBlockNext = pBlockToAllocateTo->m_pSMemBlockNext; //Set up links to this block

		if (pNewBlock->m_pSMemBlockNext)
		{
			pNewBlock->m_pSMemBlockNext->m_LeftPadding = 0;
		}

		pBlockToAllocateTo->m_pSMemBlockNext = pNewBlock;
		pBlockToAllocateTo->m_RightPadding = 0;
	}
}
/////////////////////////////////////////////////////////////////////////
// Given a start and endpoint of a block, merge with neighbouring blocks and padding
// Pointer addresses will be changed to point at the start and end of the coalesced block
/////////////////////////////////////////////////////////////////////////
void CManagedHeap::MergeWithNearbyBlocks(u8 *& pMergeStartPoint, u8 *& pMergeEndPoint)
{
	bool bCanMerge = false;
	SBlockHeader* pHeader = ((SBlockHeader*)pMergeStartPoint);
	//Merge forwards
	if (pHeader->m_pSMemBlockNext->m_bIsFreeBlock) //Merge with next if possible
	{
		bCanMerge = true;
		//Update our end pointer to merge over the other block
		pMergeEndPoint = (u8*)GetFooter(pHeader->m_pSMemBlockNext) + sizeof(SFooterBlock) + pHeader->m_pSMemBlockNext->m_RightPadding;

		pHeader->m_pSMemBlockNext = pHeader->m_pSMemBlockNext->m_pSMemBlockNext; //Update link to next block

		if (pHeader->m_pSMemBlockNext)
		{
			pHeader->m_pSMemBlockNext->m_LeftPadding = 0;//Update next block to say the padding has been taken in this merge
		}

		m_uActualFreeSpace += sizeof(SBlockHeader) + sizeof(SFooterBlock); // We have reclaimed a header and footer

	}

	////////////////////////////////////////////////////
	//Merge backwards
	SBlockHeader* pPrevHeader = GetPreviousHeader(pHeader);

	SBlockHeader* pNextBlock = pHeader->m_pSMemBlockNext;

	//If previous head exisits and is freeblock
	if (pPrevHeader && pPrevHeader->m_bIsFreeBlock)
	{
		bCanMerge = true;

		pMergeStartPoint = (u8*)pPrevHeader;
		m_uActualFreeSpace += sizeof(SBlockHeader) + sizeof(SFooterBlock);
		pHeader->m_LeftPadding = pPrevHeader->m_LeftPadding;
		pPrevHeader = GetPreviousHeader(pPrevHeader);

	}

	//Merge in padding. This is either our original starting padding
	//Or the left padding of the previous block if we are merging with it
	if (pHeader->m_LeftPadding != 0)
	{
		bCanMerge = true;
		pMergeStartPoint -= (u8)(pHeader->m_LeftPadding);
	}

	//If we determined we can merge, merge blocks
	if (bCanMerge)
	{
		SBlockHeader* newBlock = EncapsulateMemoryBlock(pMergeStartPoint, pMergeEndPoint - pMergeStartPoint);
		newBlock->m_pSMemBlockNext = pNextBlock;

		if (pPrevHeader == nullptr)
		{
			m_pBlock = newBlock;
		}
		else
		{
			pPrevHeader->m_pSMemBlockNext = newBlock;
			SFooterBlock* test = GetFooter(newBlock);

		}
#ifdef TIDYDATA
		u8* pMemoryBlock = (u8*)newBlock;
		pMemoryBlock += sizeof(SBlockHeader);
		memset(pMemoryBlock, '0', newBlock->m_uBlockSize);
#endif // TIDYDATA
	}
	else
	{
#ifdef TIDYDATA
		u8* pMemoryBlock = pMergeStartPoint;
		pMemoryBlock += sizeof(SBlockHeader);
		memset(pMemoryBlock, '0', ((SBlockHeader*)pMergeStartPoint)->m_uBlockSize);
#endif // TIDYDATA
	}
}

