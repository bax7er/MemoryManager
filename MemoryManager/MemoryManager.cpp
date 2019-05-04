#include "pch.h"
#include "String.h"
#include <chrono>
#include "CManagedHeap.h"

const u32 k_uHeapSize = 1024;

int main()
{
	CManagedHeap cMyManagedMemory;
	cMyManagedMemory.Initialise(k_uHeapSize);

	//Create some text to load to the memory manager
	std::string myText = "HELLO WORLD ";
	const char* textToPrint = myText.c_str();

	//Allocate 100 chars
	void* myAllocs[3];

	for (void*& alloc : myAllocs)
	{
		alloc = cMyManagedMemory.Allocate(sizeof(char) * 100);
		char* myChars = new(alloc) char[100];

		//Write the text to the managed memory
		for (int i = 0; i < 100; i++)
		{
			myChars[i] = textToPrint[i % myText.length()];
		}
	}
	cMyManagedMemory.Print();

	for (void*& alloc : myAllocs)
	{
		cMyManagedMemory.Deallocate(alloc);
		cMyManagedMemory.Print();
	}

	cMyManagedMemory.Shutdown();
}