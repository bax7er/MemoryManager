#ifndef _MANAGEDHEAP_H_
#define _MANAGEDHEAP_H_

#define TIDYDATA //If defined, dealocations will be overritted with blank data

////////////////////////////////////
//TypeDefs to show size in bits
// unsigned 8 bit integer
typedef unsigned char		u8;

// unsigned 32 bit integer
typedef unsigned int		u32;
////////////////////////////////////




//////////////////////////////////////////////////////////////////////////
// default alignment for windows platform is sizeof(u32)
//////////////////////////////////////////////////////////////////////////
#define _PLATFORM_MIN_ALIGN	(sizeof(u32))


class CManagedHeap
{
public:
	CManagedHeap();
	~CManagedHeap();

	//////////////////////////////////////////////////////////////////////////
	// enum of possible error return values from CManagedHeap::GetLastError()
	// These are grouped into errors relating to specific functionality
	//////////////////////////////////////////////////////////////////////////
	enum EHeapState
	{
		EHeapError_Ok = 0,						// no error

		EHeapState_Init_NotInitialised,			// Tried to use the heap, but the heap has not yet been initalised
		EHeapState_Init_UnableToAquireMemory,	// Could not aquire memory for the heap, nullptr was returned by malloc, or manually passed to initialise method
		EHeapState_Init_BadAlign,				// Memory passed Initialise wasn't aligned to _PLATFORM_MIN_ALIGN 
		EHeapState_Init_AlreadyInitialised,		// Attempted to Initialise after already being initialised successfully
		//Alloc errors
		EHeapState_Alloc_ZeroSizeAlloc,			// Allocation of 0 bytes requested - invalid
		EHeapState_Alloc_BadAlign,				// Alignment specified is not a power of 2, or smaller than the minimum allignment defined
		EHeapState_Alloc_NoLargeEnoughBlocks,	// Either the allocation is larger than the remaining memory, or there isn't a large enough free block

		EHeapState_Dealloc_Nullptr,				// Tried to deallocate a nullptr
		EHeapState_Dealloc_AlreadyDeallocated,	// Tried to deallocate a block that's already deallocated
		EHeapState_Dealloc_OverwriteUnderrun,	// Memory overwrite detected before the deallocated block
		EHeapState_Dealloc_OverwriteOverrun,	// Memory overwrite detected after the deallocated block 
	};



	// Sets up the heap by requesting memory itself from the OS
	void	Initialise(u32 uMemorySizeInBytes);

	// Sets up the heap using memory already allocated to this program
	void	Initialise(u8* pRawMemory, u32 uMemorySizeInBytes);

	// Explicit shutdown - releases memory if it was claimed by this class, call before destructor
	void	Shutdown();

	// Allocates the specified size of memory, with the specified alignment
	// and returns a pointer to it.
	void*	Allocate(u32 uNumBytes, u32 uAlignment = _PLATFORM_MIN_ALIGN);

	// deallocates the memory pointed to by pMemory and returns it to the 
	// free memory stored in the heap.
	void 	Deallocate(void* pMemory);

	// get info about the current Heap state
	inline u32		GetNumAllocs() { return m_uNumAllocations; };

	//Returns the freespace available, accounting for overheads
	u32		GetFreeMemory();

	// Returns the outcome of the last operation
	inline EHeapState GetLastError() { return m_ELastHeapError; };

	// Calcuates the offset to add to a pointer to align it to the alignment passed
	// Must be a power of two
	u32 CalculateAlignmentDelta(void* pPointerToAlign, u32 uAlignment);

	// Prints the current state of the managed memory in a friendly format
	void Print();

	// Prints the current state of the managed memory dirrectly
	void PrintDUMP();

private:

	struct SBlockHeader
	{
		SBlockHeader* m_pSMemBlockNext;
		u32 m_uBlockSize;
		bool m_bIsFreeBlock;
		u32 m_LeftPadding;
		u32 m_RightPadding;
	};

	struct SFooterBlock
	{
		SBlockHeader* m_pMatchingHeader;
		u32 m_uSizeOfBlock;
	};

	bool m_bSelfAllocatedMemory; //True if memory was allocated internally
	u8* m_pMemory;
	u32 m_uMemorySize;
	u32 m_uFreeSpace;
	u32 m_uActualFreeSpace;
	u32 m_uNumAllocations;

	SBlockHeader* m_pBlock; //First Block

	EHeapState m_ELastHeapError;

	/////////////////////////////////////////////////
	//  PRIVATE FUNCTIONS                         //
	/////////////////////////////////////////////////

	// Validates if a pointer is alligned to min platform alignment
	bool IsAligned(u8* pRawMemory);

	// Writes the footer for a given header block
	// Returns the a pointer to the footer
	SFooterBlock* WriteFooter(SBlockHeader* headerBlock);

	// Gets the position of a footer for a given header
	SFooterBlock* GetFooter(SBlockHeader* headerBlock);

	// Gets the position of the previous header, given a header
	// Will return nullptr if this is the first header
	SBlockHeader* GetPreviousHeader(SBlockHeader* headerBlock);

	// Sets up a header footer pair arround a block of a given size
	// Pointer should be pointing to where the header should be placed
	// Returns a pointer to the header created
	SBlockHeader* EncapsulateMemoryBlock(u8* pRawMemory, u32 uSizeOfBlock);

	// Itterate through the blocks, checking for a block which is free,matches our allignment, and could be large enough to allocate to
	// The block may not currently be of the corect size, but reclaiming padding will meet the requirements
	// Returns pointer to first block which satisfies the criteria, nullptr otherwise
	SBlockHeader* FindFreeBlock(u32 uSizeOfBlockToFind, u32 uAlignment);

	// Validates if a given unsigned interger is a power of two
	bool IsPowerOfTwo(u32 uNumberToTest);

	// Determines if a block is viable, given the block to check, the size of the allocation and the alignment required
	// This function takes into account reclaiming padding for this block
	bool IsBlockViable(SBlockHeader* pBlockToCheck, u32 uSizeOfBlockToFind, u32 uAlignment);

	// Moves the position of the header to correct allignment, reclaiming or adding padding if required
	// WILL CHANGE THE ADDRESS OF THE POINTER, therefore the varible itself is passed by reference
	void AdjustBlockPositionForPadding(u32 uAlignment, SBlockHeader*& pBlockToAllocateTo);

	// Evaluates the free space after our allocation, and if the space is large enough,
	// encapsulating it with a header and footer. If not, the data is marked as padding to be reclaimed later
	void ManageFreeSpacePostAllocation(SBlockHeader* pBlockToAllocateTo, u32 uNumBytes);

	// Given a start and endpoint of a block, merge with neighbouring blocks and padding
// Pointer addresses will be changed to point at the start and end of the coalesced block
	void MergeWithNearbyBlocks(u8*& pMergeStartPoint, u8*& pMergeEndPoint);
};
#endif // #ifndef _MANAGEDHEAP_H_