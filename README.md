# MemoryManager
The purpose of the project was to gain experience with working with pointers and an understand oh what steps going into managing memory.

The project involves a class to act as the heap, with the ability to request memory from it with a given byte boundary, release memory back too it and query the current state for available memory. Overwrite and underwrite detection is also included, as well as the ability to visualise the current state of the heap.


Allocations are made using a first fit algorithm, with a small overhead for a header and footer, encapsulating each memory allocation and free space. While this overhead can be inefficient with a large number of small allocations, it allows the heap to scale well to larger heap sizes.

De-allocations coalesce with nearby free memory blocks to reduce memory fragmentation and to remove obsolete headers.

The memory manager is limited to a smaller heap size as a 32 bit unsigned integer is used to represent the number of bytes, limiting the total heap size to 4,294,967,295 bytes, or approximately 4 Gigabytes. Additionally I recognise the header structure could be optimised further, as each header and footer pair takes up 32 bytes.
