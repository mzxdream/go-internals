// Copyright 2009 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

// Garbage collector.

#include "runtime.h"
#include "arch_GOARCH.h"
#include "malloc.h"
#include "stack.h"
#include "mgc0.h"
#include "race.h"
#include "type.h"
#include "typekind.h"
#include "hashmap.h"

enum {
	Debug = 0,
	DebugMark = 0,  // run second pass to check mark
	CollectStats = 0,

	// Four bits per word (see #defines below).
	wordsPerBitmapWord = sizeof(void*)*8/4,
	bitShift = sizeof(void*)*8/4,

	handoffThreshold = 4,
	IntermediateBufferCapacity = 64,

	// Bits in type information
	PRECISE = 1,
	LOOP = 2,
	PC_BITS = PRECISE | LOOP,
};

/* 每个机器字节(32位或64位)会对应4位的标记位.因此相当于64位系统中每个标记位图的字节对应16个堆字节.

   字节中的位先根据类型,再根据堆中的分配位置进行打包,因此每个64位的标记位图从上到下依次包括:
	16位特殊位,对应堆字节
	16位垃圾回收的标记位
	16字节的 无指针/块边界 的标记位
	16位的 已分配 标记位
   这样设计使得对一个类型的相应的位进行遍历很容易.

   地址与它们的标记位图是分开存储和.以mheap.arena_start地址为边界,向上是实际的地址空间,向下是标记位图.
   比如在64位系统中,计算某个地址的标记位的公式如下:
	偏移 = 地址 - mheap.arena_start
	标记位地址 = mheap.arena_start - 偏移/16 - 1 
	移位 = 偏移 % 16
	标记位 = *标记位地址 >> 移位
*/
// Bits in per-word bitmap.
// #defines because enum might not be able to hold the values.
//
// Each word in the bitmap describes wordsPerBitmapWord words
// of heap memory.  There are 4 bitmap bits dedicated to each heap word,
// so on a 64-bit system there is one bitmap word per 16 heap words.
// The bits in the word are packed together by type first, then by
// heap location, so each 64-bit bitmap word consists of, from top to bottom,
// the 16 bitSpecial bits for the corresponding heap words, then the 16 bitMarked bits,
// then the 16 bitNoPointers/bitBlockBoundary bits, then the 16 bitAllocated bits.
// This layout makes it easier to iterate over the bits of a given type.
//
// The bitmap starts at mheap.arena_start and extends *backward* from
// there.  On a 64-bit system the off'th word in the arena is tracked by
// the off/16+1'th word before mheap.arena_start.  (On a 32-bit system,
// the only difference is that the divisor is 8.)
//
// To pull out the bits corresponding to a given pointer p, we use:
//
//	off = p - (uintptr*)mheap.arena_start;  // word offset
//	b = (uintptr*)mheap.arena_start - off/wordsPerBitmapWord - 1;
//	shift = off % wordsPerBitmapWord
//	bits = *b >> shift;
//	/* then test bits & bitAllocated, bits & bitMarked, etc. */
//
#define bitAllocated		((uintptr)1<<(bitShift*0))
#define bitNoPointers		((uintptr)1<<(bitShift*1))	/* when bitAllocated is set */
#define bitMarked		((uintptr)1<<(bitShift*2))	/* when bitAllocated is set */
#define bitSpecial		((uintptr)1<<(bitShift*3))	/* when bitAllocated is set - has finalizer or being profiled */
#define bitBlockBoundary	((uintptr)1<<(bitShift*1))	/* when bitAllocated is NOT set */

#define bitMask (bitBlockBoundary | bitAllocated | bitMarked | bitSpecial)

// Holding worldsema grants an M the right to try to stop the world.
// The procedure is:
//
//	runtime·semacquire(&runtime·worldsema);
//	m->gcing = 1;
//	runtime·stoptheworld();
//
//	... do stuff ...
//
//	m->gcing = 0;
//	runtime·semrelease(&runtime·worldsema);
//	runtime·starttheworld();
//
uint32 runtime·worldsema = 1;

static int32 gctrace;

typedef struct Obj Obj;
struct Obj
{
	byte	*p;	// data pointer
	uintptr	n;	// size of data in bytes
	/* 在文件type.h中，type结构体中有一个void *gc,是一个指向操作码的指针，gc过程中对不同的类型根据操作码进行gc操作
	   相当于内部有一个小的虚报机实现
	 */
	uintptr	ti;	// type info 
};

// Workbuf的大小是N倍的内存页大小，放的是Obj的数组
typedef struct Workbuf Workbuf;
struct Workbuf
{
#define SIZE (2*PageSize-sizeof(LFNode)-sizeof(uintptr))
	LFNode  node; // must be first
	uintptr nobj;
	Obj     obj[SIZE/sizeof(Obj) - 1];
	uint8   _padding[SIZE%sizeof(Obj) + sizeof(Obj)];
#undef SIZE
};

typedef struct Finalizer Finalizer;
struct Finalizer
{
	FuncVal *fn;
	void *arg;
	uintptr nret;
};

// 多个Finalizer组成一个FinBlock
typedef struct FinBlock FinBlock;
struct FinBlock
{
	FinBlock *alllink;
	FinBlock *next;
	int32 cnt;
	int32 cap;
	Finalizer fin[1];
};

extern byte data[];
extern byte edata[];
extern byte bss[];
extern byte ebss[];

extern byte gcdata[];
extern byte gcbss[];

static G *fing;
static FinBlock *finq; // list of finalizers that are to be executed
static FinBlock *finc; // cache of free blocks
static FinBlock *allfin; // list of all blocks
static Lock finlock;
static int32 fingwait;

static void runfinq(void);
static Workbuf* getempty(Workbuf*);
static Workbuf* getfull(Workbuf*);
static void	putempty(Workbuf*);
static Workbuf* handoff(Workbuf*);

static struct {
	uint64	full;  // lock-free list of full blocks
	uint64	empty; // lock-free list of empty blocks
	byte	pad0[CacheLineSize]; // prevents false-sharing between full/empty and nproc/nwait //阻止full/empty和nproc/nwait之间伪共享
	uint32	nproc;
	volatile uint32	nwait;
	volatile uint32	ndone;
	volatile uint32 debugmarkdone;
	Note	alldone;
	ParFor	*markfor;
	ParFor	*sweepfor;

	Lock;
	byte	*chunk;
	uintptr	nchunk;

	Obj	*roots;
	uint32	nroot;
	uint32	rootcap;
} work;

enum {
	GC_DEFAULT_PTR = GC_NUM_INSTR,
	GC_MAP_NEXT,
	GC_CHAN,

	GC_NUM_INSTR2
};

static struct {
	struct {
		uint64 sum;
		uint64 cnt;
	} ptr;
	uint64 nbytes;
	struct {
		uint64 sum;
		uint64 cnt;
		uint64 notype;
		uint64 typelookup;
	} obj;
	uint64 rescan;
	uint64 rescanbytes;
	uint64 instr[GC_NUM_INSTR2];
	uint64 putempty;
	uint64 getfull;
} gcstats;

// markonly marks an object. It returns true if the object
// has been marked by this function, false otherwise.
// This function isn't thread-safe and doesn't append the object to any buffer.
static bool
markonly(void *obj)
{
	byte *p;
	uintptr *bitp, bits, shift, x, xbits, off;
	MSpan *s;
	PageID k;

	// Words outside the arena cannot be pointers.
	if(obj < runtime·mheap->arena_start || obj >= runtime·mheap->arena_used)
		return false;

	// obj may be a pointer to a live object.
	// Try to find the beginning of the object.

	/* 由给定的指针找到对应的对象:先将地址取整到字边界
	   再在bitmap查找，看相应的位是否是分配且BlockBoundary
	 */
	// Round down to word boundary.
	obj = (void*)((uintptr)obj & ~((uintptr)PtrSize-1));

	// Find bits for this word.
	off = (uintptr*)obj - (uintptr*)runtime·mheap->arena_start;
	bitp = (uintptr*)runtime·mheap->arena_start - off/wordsPerBitmapWord - 1;
	shift = off % wordsPerBitmapWord;
	xbits = *bitp;
	bits = xbits >> shift;

	// Pointing at the beginning of a block?
	if((bits & (bitAllocated|bitBlockBoundary)) != 0)
		goto found;

	// Otherwise consult span table to find beginning.
	// (Manually inlined copy of MHeap_LookupMaybe.)
	/* 下面这段代码跟MHeap_LookupMaybe功能一样的，就是给定一个指针，查找对应的MSpan */
	k = (uintptr)obj>>PageShift;
	x = k;
	if(sizeof(void*) == 8)
		x -= (uintptr)runtime·mheap->arena_start>>PageShift;
	s = runtime·mheap->map[x];
	if(s == nil || k < s->start || k - s->start >= s->npages || s->state != MSpanInUse)
		return false;
	/* 再由MSpan的对象尺寸类别，得到指针的对象边界 */
	p = (byte*)((uintptr)s->start<<PageShift);
	if(s->sizeclass == 0) {
		obj = p;
	} else {
		if((byte*)obj >= (byte*)s->limit)
			return false;
		uintptr size = s->elemsize;
		int32 i = ((byte*)obj - p)/size;
		obj = p+i*size;
	}

	/* 得到对象头地址之后，重新加载位图中的标记位 */
	// Now that we know the object header, reload bits.
	off = (uintptr*)obj - (uintptr*)runtime·mheap->arena_start;
	bitp = (uintptr*)runtime·mheap->arena_start - off/wordsPerBitmapWord - 1;
	shift = off % wordsPerBitmapWord;
	xbits = *bitp;
	bits = xbits >> shift;

found:
	// Now we have bits, bitp, and shift correct for
	// obj pointing at the base of the object.
	// Only care about allocated and not marked.
	if((bits & (bitAllocated|bitMarked)) != bitAllocated)
		return false;
	/* 将Marked位置位以标记该对象 */
	*bitp |= bitMarked<<shift;

	// The object is now marked
	return true;
}

// PtrTarget and BitTarget are structures used by intermediate buffers.
// The intermediate buffers hold GC data before it
// is moved/flushed to the work buffer (Workbuf).
// The size of an intermediate buffer is very small,
// such as 32 or 64 elements.
/* PtrTarget和BitTarget是临时缓存使用到的数据结构。
   垃圾回收的数据在move/flush到工作缓存之前先放在临时缓存中
   临时缓存大小很小，大概只有32或者64个。
 */
typedef struct PtrTarget PtrTarget;
struct PtrTarget
{
	void *p;
	uintptr ti;
};

typedef struct BitTarget BitTarget;
struct BitTarget
{
	void *p;
	uintptr ti;
	uintptr *bitp, shift;
};

typedef struct BufferList BufferList;
struct BufferList
{
	PtrTarget ptrtarget[IntermediateBufferCapacity];//大小为64个的数组
	BitTarget bittarget[IntermediateBufferCapacity];
	Obj obj[IntermediateBufferCapacity];
	BufferList *next;
};
static BufferList *bufferList;

static Lock lock;
static Type *itabtype;

static void enqueue(Obj obj, Workbuf **_wbuf, Obj **_wp, uintptr *_nobj);

// flushptrbuf moves data from the PtrTarget buffer to the work buffer.
// The PtrTarget buffer contains blocks irrespective of whether the blocks have been marked or scanned,
// while the work buffer contains blocks which have been marked
// and are prepared to be scanned by the garbage collector.
/* PtrTarget缓存中包括的块与该块是否被标记或扫描过无关。
   而工作缓存中包含的被标记过的块，并且是准备给垃圾收集器扫描的
 */
//
// _wp, _wbuf, _nobj are input/output parameters and are specifying the work buffer.
// bitbuf holds temporary data generated by this function.
//
// A simplified drawing explaining how the todo-list moves from a structure to another:
//
//     scanblock
//  (find pointers)
//    Obj ------> PtrTarget (pointer targets)
//     ↑          |
//     |          | flushptrbuf (1st part,
//     |          | find block start)
//     |          ↓
//     `--------- BitTarget (pointer targets and the corresponding locations in bitmap)
//  flushptrbuf
//  (2nd part, mark and enqueue)
static void
flushptrbuf(PtrTarget *ptrbuf, PtrTarget **ptrbufpos, Obj **_wp, Workbuf **_wbuf, uintptr *_nobj, BitTarget *bitbuf)
{
	byte *p, *arena_start, *obj;
	uintptr size, *bitp, bits, shift, j, x, xbits, off, nobj, ti, n;
	MSpan *s;
	PageID k;
	Obj *wp;
	Workbuf *wbuf;
	PtrTarget *ptrbuf_end;
	BitTarget *bitbufpos, *bt;

	arena_start = runtime·mheap->arena_start;

	wp = *_wp;
	wbuf = *_wbuf;
	nobj = *_nobj;

	ptrbuf_end = *ptrbufpos;
	n = ptrbuf_end - ptrbuf;
	*ptrbufpos = ptrbuf;

	if(CollectStats) {
		runtime·xadd64(&gcstats.ptr.sum, n);
		runtime·xadd64(&gcstats.ptr.cnt, 1);
	}

	// If buffer is nearly full, get a new one.
	if(wbuf == nil || nobj+n >= nelem(wbuf->obj)) {
		if(wbuf != nil)
			wbuf->nobj = nobj;
		wbuf = getempty(wbuf);
		wp = wbuf->obj;
		nobj = 0;

		if(n >= nelem(wbuf->obj))
			runtime·throw("ptrbuf has to be smaller than WorkBuf");
	}

	// TODO(atom): This block is a branch of an if-then-else statement.
	//             The single-threaded branch may be added in a next CL.
	{
		// Multi-threaded version.

		bitbufpos = bitbuf;

		/* 接下来的这个while的代码是从PtrTarget到BitTarget
		   通过对象的指针找到相应的标记位,确定是否要将它放到bitbuff中
		 */
		while(ptrbuf < ptrbuf_end) {
			obj = ptrbuf->p;
			ti = ptrbuf->ti;
			ptrbuf++;

			// obj belongs to interval [mheap.arena_start, mheap.arena_used).
			if(Debug > 1) {
				if(obj < runtime·mheap->arena_start || obj >= runtime·mheap->arena_used)
					runtime·throw("object is outside of mheap");
			}

			// obj may be a pointer to a live object.
			// Try to find the beginning of the object.
			/* 定位到object的头部 */
			// Round down to word boundary.
			if(((uintptr)obj & ((uintptr)PtrSize-1)) != 0) {
				obj = (void*)((uintptr)obj & ~((uintptr)PtrSize-1));
				ti = 0;
			}

			// Find bits for this word.找到这个字对应的位图的标记位
			off = (uintptr*)obj - (uintptr*)arena_start;
			bitp = (uintptr*)arena_start - off/wordsPerBitmapWord - 1;
			shift = off % wordsPerBitmapWord;
			xbits = *bitp;
			bits = xbits >> shift;

			// Pointing at the beginning of a block?  看是否是分配且在块的头部,是的说明找到
			if((bits & (bitAllocated|bitBlockBoundary)) != 0)
				goto found;

			ti = 0;

			// Pointing just past the beginning?
			// Scan backward a little to find a block boundary.直到找到块边界
			for(j=shift; j-->0; ) {
				if(((xbits>>j) & (bitAllocated|bitBlockBoundary)) != 0) {
					obj = (byte*)obj - (shift-j)*PtrSize;
					shift = j;
					bits = xbits>>shift;
					goto found;
				}
			}

			/* 如果没有找到,则找地址所在的MSpan,然后通MSpan找地址所处的对象的边界
			 */
			// Otherwise consult span table to find beginning.
			// (Manually inlined copy of MHeap_LookupMaybe.)
			k = (uintptr)obj>>PageShift;
			x = k;
			if(sizeof(void*) == 8)
				x -= (uintptr)arena_start>>PageShift;
			s = runtime·mheap->map[x];
			if(s == nil || k < s->start || k - s->start >= s->npages || s->state != MSpanInUse)
				continue;
			p = (byte*)((uintptr)s->start<<PageShift);
			if(s->sizeclass == 0) {
				obj = p;
			} else {
				if((byte*)obj >= (byte*)s->limit)
					continue;
				size = s->elemsize;
				int32 i = ((byte*)obj - p)/size;
				obj = p+i*size;
			}

			// Now that we know the object header, reload bits.找到对象边界后,重新加载标记位
			off = (uintptr*)obj - (uintptr*)arena_start;
			bitp = (uintptr*)arena_start - off/wordsPerBitmapWord - 1;
			shift = off % wordsPerBitmapWord;
			xbits = *bitp;
			bits = xbits >> shift;

		found:
			// Now we have bits, bitp, and shift correct for
			// obj pointing at the base of the object.
			// Only care about allocated and not marked.
			/* 找到了.确定它是标了分配位,没有标垃圾回收位的标记,则不用管
			   否则要加到bitbuff中
			 */
			if((bits & (bitAllocated|bitMarked)) != bitAllocated)
				continue;

			*bitbufpos++ = (BitTarget){obj, ti, bitp, shift};
		}

		/* 接下来的代码是将BitTarget进行标记,加上垃圾回收位
		 */
		runtime·lock(&lock);
		for(bt=bitbuf; bt<bitbufpos; bt++){
			xbits = *bt->bitp;
			bits = xbits >> bt->shift;
			if((bits & bitMarked) != 0)//已经有mark位的不用管
				continue;

			// Mark the block 否则要将块进行标记
			*bt->bitp = xbits | (bitMarked << bt->shift);

			// If object has no pointers, don't need to scan further.
			/* 如果这个对象中不包含指针,则不会引用到其它对象.将它自身的垃圾回收位标上就可以了
			   否则,还要将从它出去的指针放到work缓存中递归地进行标记
			 */
			if((bits & bitNoPointers) != 0)
				continue;

			obj = bt->p;

			// Ask span about size class.
			// (Manually inlined copy of MHeap_Lookup.)
			x = (uintptr)obj >> PageShift;
			if(sizeof(void*) == 8)
				x -= (uintptr)arena_start>>PageShift;
			s = runtime·mheap->map[x];

			PREFETCH(obj);

			//放到work缓存中
			*wp = (Obj){obj, s->elemsize, bt->ti};
			wp++;
			nobj++;
		}
		runtime·unlock(&lock);

		// If another proc wants a pointer, give it some.
		if(work.nwait > 0 && nobj > handoffThreshold && work.full == 0) {
			wbuf->nobj = nobj;
			wbuf = handoff(wbuf);
			nobj = wbuf->nobj;
			wp = wbuf->obj + nobj;
		}
	}

	*_wp = wp;
	*_wbuf = wbuf;
	*_nobj = nobj;
}

static void
flushobjbuf(Obj *objbuf, Obj **objbufpos, Obj **_wp, Workbuf **_wbuf, uintptr *_nobj)
{
	uintptr nobj, off;
	Obj *wp, obj;
	Workbuf *wbuf;
	Obj *objbuf_end;

	wp = *_wp;
	wbuf = *_wbuf;
	nobj = *_nobj;

	objbuf_end = *objbufpos;
	*objbufpos = objbuf;

	while(objbuf < objbuf_end) {
		obj = *objbuf++;

		// Align obj.b to a word boundary.
		off = (uintptr)obj.p & (PtrSize-1);
		if(off != 0) {
			obj.p += PtrSize - off;
			obj.n -= PtrSize - off;
			obj.ti = 0;
		}

		if(obj.p == nil || obj.n == 0)
			continue;

		// If buffer is full, get a new one.
		if(wbuf == nil || nobj >= nelem(wbuf->obj)) {
			if(wbuf != nil)
				wbuf->nobj = nobj;
			wbuf = getempty(wbuf);
			wp = wbuf->obj;
			nobj = 0;
		}

		*wp = obj;
		wp++;
		nobj++;
	}

	// If another proc wants a pointer, give it some.
	if(work.nwait > 0 && nobj > handoffThreshold && work.full == 0) {
		wbuf->nobj = nobj;
		wbuf = handoff(wbuf);
		nobj = wbuf->nobj;
		wp = wbuf->obj + nobj;
	}

	*_wp = wp;
	*_wbuf = wbuf;
	*_nobj = nobj;
}

// Program that scans the whole block and treats every block element as a potential pointer
static uintptr defaultProg[2] = {PtrSize, GC_DEFAULT_PTR};

// Hashmap iterator program
static uintptr mapProg[2] = {0, GC_MAP_NEXT};

// Hchan program
static uintptr chanProg[2] = {0, GC_CHAN};

/* 这边的几个都是虚拟机的指令。这个Frame结构体其实是虚拟机的栈。
   垃圾回收的工作由执行相应的操作码完成
 */
// Local variables of a program fragment or loop
typedef struct Frame Frame;
struct Frame {
	uintptr count, elemsize, b;
	uintptr *loop_or_ret;
};

// scanblock scans a block of n bytes starting at pointer b for references
// to other objects, scanning any it finds recursively until there are no
// unscanned objects left.  Instead of using an explicit recursion, it keeps
// a work list in the Workbuf* structures and loops in the main function
// body.  Keeping an explicit work list is easier on the stack allocator and
// more efficient.
//
// wbuf: current work buffer
// wp:   storage for next queued pointer (write pointer)
// nobj: number of queued objects
static void
scanblock(Workbuf *wbuf, Obj *wp, uintptr nobj, bool keepworking)
{
	byte *b, *arena_start, *arena_used;
	uintptr n, i, end_b, elemsize, size, ti, objti, count, type;
	uintptr *pc, precise_type, nominal_size;
	uintptr *map_ret, mapkey_size, mapval_size, mapkey_ti, mapval_ti;
	void *obj;
	Type *t;
	Slice *sliceptr;
	Frame *stack_ptr, stack_top, stack[GC_STACK_CAPACITY+4];
	BufferList *scanbuffers;
	PtrTarget *ptrbuf, *ptrbuf_end, *ptrbufpos;
	BitTarget *bitbuf;
	Obj *objbuf, *objbuf_end, *objbufpos;
	Eface *eface;
	Iface *iface;
	Hmap *hmap;
	MapType *maptype;
	bool didmark, mapkey_kind, mapval_kind;
	struct hash_gciter map_iter;
	struct hash_gciter_data d;
	Hchan *chan;
	ChanType *chantype;

	if(sizeof(Workbuf) % PageSize != 0)
		runtime·throw("scanblock: size of Workbuf is suboptimal");

	// Memory arena parameters.
	arena_start = runtime·mheap->arena_start;
	arena_used = runtime·mheap->arena_used;

	stack_ptr = stack+nelem(stack)-1;
	
	precise_type = false;
	nominal_size = 0;

	// Allocate ptrbuf, bitbuf
	/* 一个bufferList中有64个ptrTarget和64个bitTarget，还有64个Obj对象
	   bufferList是链起来的
	 */
	{
		runtime·lock(&lock);

		if(bufferList == nil) {
			bufferList = runtime·SysAlloc(sizeof(*bufferList));
			if(bufferList == nil)
				runtime·throw("runtime: cannot allocate memory");
			bufferList->next = nil;
		}
		scanbuffers = bufferList;
		bufferList = bufferList->next;

		ptrbuf = &scanbuffers->ptrtarget[0];
		ptrbuf_end = &scanbuffers->ptrtarget[0] + nelem(scanbuffers->ptrtarget);
		bitbuf = &scanbuffers->bittarget[0];
		objbuf = &scanbuffers->obj[0];
		objbuf_end = &scanbuffers->obj[0] + nelem(scanbuffers->obj);

		runtime·unlock(&lock);
	}

	ptrbufpos = ptrbuf;
	objbufpos = objbuf;

	// (Silence the compiler)
	map_ret = nil;
	mapkey_size = mapval_size = 0;
	mapkey_kind = mapval_kind = false;
	mapkey_ti = mapval_ti = 0;
	chan = nil;
	chantype = nil;

	goto next_block;

	for(;;) {
		// Each iteration scans the block b of length n, queueing pointers in
		// the work buffer.
		if(Debug > 1) {
			runtime·printf("scanblock %p %D\n", b, (int64)n);
		}

		if(CollectStats) {
			runtime·xadd64(&gcstats.nbytes, n);
			runtime·xadd64(&gcstats.obj.sum, nobj);
			runtime·xadd64(&gcstats.obj.cnt, 1);
		}

		if(ti != 0) {
			/* ti是类型信息typeinfo的一个标记位
			   pc是类型信息中的虚拟机的代码段
			 */
			pc = (uintptr*)(ti & ~(uintptr)PC_BITS);
			precise_type = (ti & PRECISE);
			stack_top.elemsize = pc[0];
			if(!precise_type)
				nominal_size = pc[0];
			if(ti & LOOP) {
				stack_top.count = 0;	// 0 means an infinite number of iterations
				stack_top.loop_or_ret = pc+1;
			} else {
				stack_top.count = 1;
			}
		} else if(UseSpanType) {
			if(CollectStats)
				runtime·xadd64(&gcstats.obj.notype, 1);

			type = runtime·gettype(b);//通过地址b，找到它的MSpan，然后看MSpan的type.compression
			if(type != 0) {
				if(CollectStats)
					runtime·xadd64(&gcstats.obj.typelookup, 1);

				/* type的低位是类型信息，通过type & (PtrSize-1)得到
				   type的高位是存的一个地址，该地址指向一个Type结构体。因为按字节对齐的，这是一种小的hack技巧哦~~
				 */
				t = (Type*)(type & ~(uintptr)(PtrSize-1));
				switch(type & (PtrSize-1)) {
				case TypeInfo_SingleObject:
					pc = (uintptr*)t->gc;
					precise_type = true;  // type information about 'b' is precise
					stack_top.count = 1;
					stack_top.elemsize = pc[0];
					break;
				case TypeInfo_Array:
					pc = (uintptr*)t->gc;
					if(pc[0] == 0)
						goto next_block;
					precise_type = true;  // type information about 'b' is precise
					stack_top.count = 0;  // 0 means an infinite number of iterations
					stack_top.elemsize = pc[0];
					stack_top.loop_or_ret = pc+1;
					break;
				case TypeInfo_Map:
					hmap = (Hmap*)b;
					maptype = (MapType*)t;
					if(hash_gciter_init(hmap, &map_iter)) {
						mapkey_size = maptype->key->size;
						mapkey_kind = maptype->key->kind;
						mapkey_ti   = (uintptr)maptype->key->gc | PRECISE;
						mapval_size = maptype->elem->size;
						mapval_kind = maptype->elem->kind;
						mapval_ti   = (uintptr)maptype->elem->gc | PRECISE;

						map_ret = 0;
						pc = mapProg;
					} else {
						goto next_block;
					}
					break;
				case TypeInfo_Chan:
					chan = (Hchan*)b;
					chantype = (ChanType*)t;
					pc = chanProg;
					break;
				default:
					runtime·throw("scanblock: invalid type");
					return;
				}
			} else {
				pc = defaultProg;
			}
		} else {
			pc = defaultProg;
		}

		pc++;
		stack_top.b = (uintptr)b;

		end_b = (uintptr)b + n - PtrSize;

	/* 下面这段开始是实现的一个虚拟机。垃圾回收的指令全部变成了操作码，相当于可执行文件的代码段
	   存放在typeinfo->gc中。每读一个操作码指令，执行对应的操作
	*/
	for(;;) {
		if(CollectStats)
			runtime·xadd64(&gcstats.instr[pc[0]], 1);

		obj = nil;
		objti = 0;
		switch(pc[0]) {
		case GC_PTR:
			obj = *(void**)(stack_top.b + pc[1]);
			objti = pc[2];
			pc += 3;
			break;

		case GC_SLICE:
			sliceptr = (Slice*)(stack_top.b + pc[1]);
			if(sliceptr->cap != 0) {
				obj = sliceptr->array;
				objti = pc[2] | PRECISE | LOOP;
			}
			pc += 3;
			break;

		case GC_APTR:
			obj = *(void**)(stack_top.b + pc[1]);
			pc += 2;
			break;

		case GC_STRING:
			obj = *(void**)(stack_top.b + pc[1]);
			pc += 2;
			break;

		case GC_EFACE:
			eface = (Eface*)(stack_top.b + pc[1]);
			pc += 2;
			if(eface->type != nil && (eface->data >= arena_start && eface->data < arena_used)) {
				t = eface->type;
				if(t->size <= sizeof(void*)) {
					if((t->kind & KindNoPointers))
						break;

					obj = eface->data;
					if((t->kind & ~KindNoPointers) == KindPtr)
						objti = (uintptr)((PtrType*)t)->elem->gc;
				} else {
					obj = eface->data;
					objti = (uintptr)t->gc;
				}
			}
			break;

		case GC_IFACE:
			iface = (Iface*)(stack_top.b + pc[1]);
			pc += 2;
			if(iface->tab == nil)
				break;
			
			// iface->tab
			if((void*)iface->tab >= arena_start && (void*)iface->tab < arena_used) {
				*ptrbufpos++ = (PtrTarget){iface->tab, (uintptr)itabtype->gc};
				if(ptrbufpos == ptrbuf_end)
					flushptrbuf(ptrbuf, &ptrbufpos, &wp, &wbuf, &nobj, bitbuf);
			}

			// iface->data
			if(iface->data >= arena_start && iface->data < arena_used) {
				t = iface->tab->type;
				if(t->size <= sizeof(void*)) {
					if((t->kind & KindNoPointers))
						break;

					obj = iface->data;
					if((t->kind & ~KindNoPointers) == KindPtr)
						objti = (uintptr)((PtrType*)t)->elem->gc;
				} else {
					obj = iface->data;
					objti = (uintptr)t->gc;
				}
			}
			break;

		case GC_DEFAULT_PTR:
			while((i = stack_top.b) <= end_b) {
				stack_top.b += PtrSize;
				obj = *(byte**)i;
				if(obj >= arena_start && obj < arena_used) {
					*ptrbufpos++ = (PtrTarget){obj, 0};
					if(ptrbufpos == ptrbuf_end)
						flushptrbuf(ptrbuf, &ptrbufpos, &wp, &wbuf, &nobj, bitbuf);
				}
			}
			goto next_block;

		case GC_END:
			if(--stack_top.count != 0) {
				// Next iteration of a loop if possible.
				elemsize = stack_top.elemsize;
				stack_top.b += elemsize;
				if(stack_top.b + elemsize <= end_b+PtrSize) {
					pc = stack_top.loop_or_ret;
					continue;
				}
				i = stack_top.b;
			} else {
				// Stack pop if possible.
				if(stack_ptr+1 < stack+nelem(stack)) {
					pc = stack_top.loop_or_ret;
					stack_top = *(++stack_ptr);
					continue;
				}
				i = (uintptr)b + nominal_size;
			}
			if(!precise_type) {
				// Quickly scan [b+i,b+n) for possible pointers.
				for(; i<=end_b; i+=PtrSize) {
					if(*(byte**)i != nil) {
						// Found a value that may be a pointer.
						// Do a rescan of the entire block.
						enqueue((Obj){b, n, 0}, &wbuf, &wp, &nobj);
						if(CollectStats) {
							runtime·xadd64(&gcstats.rescan, 1);
							runtime·xadd64(&gcstats.rescanbytes, n);
						}
						break;
					}
				}
			}
			goto next_block;

		case GC_ARRAY_START:
			i = stack_top.b + pc[1];
			count = pc[2];
			elemsize = pc[3];
			pc += 4;

			// Stack push.
			*stack_ptr-- = stack_top;
			stack_top = (Frame){count, elemsize, i, pc};
			continue;

		case GC_ARRAY_NEXT:
			if(--stack_top.count != 0) {
				stack_top.b += stack_top.elemsize;
				pc = stack_top.loop_or_ret;
			} else {
				// Stack pop.
				stack_top = *(++stack_ptr);
				pc += 1;
			}
			continue;

		case GC_CALL:
			// Stack push.
			*stack_ptr-- = stack_top;
			stack_top = (Frame){1, 0, stack_top.b + pc[1], pc+3 /*return address*/};
			pc = (uintptr*)((byte*)pc + *(int32*)(pc+2));  // target of the CALL instruction
			continue;

		case GC_MAP_PTR:
			hmap = *(Hmap**)(stack_top.b + pc[1]);
			if(hmap == nil) {
				pc += 3;
				continue;
			}
			runtime·lock(&lock);
			didmark = markonly(hmap);
			runtime·unlock(&lock);
			if(didmark) {
				maptype = (MapType*)pc[2];
				if(hash_gciter_init(hmap, &map_iter)) {
					mapkey_size = maptype->key->size;
					mapkey_kind = maptype->key->kind;
					mapkey_ti   = (uintptr)maptype->key->gc | PRECISE;
					mapval_size = maptype->elem->size;
					mapval_kind = maptype->elem->kind;
					mapval_ti   = (uintptr)maptype->elem->gc | PRECISE;

					// Start mapProg.
					map_ret = pc+3;
					pc = mapProg+1;
				} else {
					pc += 3;
				}
			} else {
				pc += 3;
			}
			continue;

		case GC_MAP_NEXT:
			// Add all keys and values to buffers, mark all subtables.
			while(hash_gciter_next(&map_iter, &d)) {
				// buffers: reserve space for 2 objects.
				if(ptrbufpos+2 >= ptrbuf_end)
					flushptrbuf(ptrbuf, &ptrbufpos, &wp, &wbuf, &nobj, bitbuf);
				if(objbufpos+2 >= objbuf_end)
					flushobjbuf(objbuf, &objbufpos, &wp, &wbuf, &nobj);

				if(d.st != nil) {
					runtime·lock(&lock);
					markonly(d.st);
					runtime·unlock(&lock);
				}
				if(d.key_data != nil) {
					if(!(mapkey_kind & KindNoPointers) || d.indirectkey) {
						if(!d.indirectkey)
							*objbufpos++ = (Obj){d.key_data, mapkey_size, mapkey_ti};
						else
							*ptrbufpos++ = (PtrTarget){*(void**)d.key_data, mapkey_ti};
					}
					if(!(mapval_kind & KindNoPointers) || d.indirectval) {
						if(!d.indirectval)
							*objbufpos++ = (Obj){d.val_data, mapval_size, mapval_ti};
						else
							*ptrbufpos++ = (PtrTarget){*(void**)d.val_data, mapval_ti};
					}
				}
			}
			if(map_ret == 0)
				goto next_block;
			pc = map_ret;
			continue;

		case GC_REGION:
			obj = (void*)(stack_top.b + pc[1]);
			size = pc[2];
			objti = pc[3];
			pc += 4;

			*objbufpos++ = (Obj){obj, size, objti};
			if(objbufpos == objbuf_end)
				flushobjbuf(objbuf, &objbufpos, &wp, &wbuf, &nobj);
			break;

		case GC_CHAN:
			// There are no heap pointers in struct Hchan,
			// so we can ignore the leading sizeof(Hchan) bytes.
			if(!(chantype->elem->kind & KindNoPointers)) {
				// Channel's buffer follows Hchan immediately in memory.
				// Size of buffer (cap(c)) is second int in the chan struct.
				n = ((uintgo*)chan)[1];
				if(n > 0) {
					// TODO(atom): split into two chunks so that only the
					// in-use part of the circular buffer is scanned.
					// (Channel routines zero the unused part, so the current
					// code does not lead to leaks, it's just a little inefficient.)
					*objbufpos++ = (Obj){(byte*)chan+runtime·Hchansize, n*chantype->elem->size,
						(uintptr)chantype->elem->gc | PRECISE | LOOP};
					if(objbufpos == objbuf_end)
						flushobjbuf(objbuf, &objbufpos, &wp, &wbuf, &nobj);
				}
			}
			goto next_block;

		default:
			runtime·throw("scanblock: invalid GC instruction");
			return;
		}

		if(obj >= arena_start && obj < arena_used) {
			*ptrbufpos++ = (PtrTarget){obj, objti};
			if(ptrbufpos == ptrbuf_end)
				flushptrbuf(ptrbuf, &ptrbufpos, &wp, &wbuf, &nobj, bitbuf);
		}
	}

	next_block:
		// Done scanning [b, b+n).  Prepare for the next iteration of
		// the loop by setting b, n, ti to the parameters for the next block.

		if(nobj == 0) {
			flushptrbuf(ptrbuf, &ptrbufpos, &wp, &wbuf, &nobj, bitbuf);
			flushobjbuf(objbuf, &objbufpos, &wp, &wbuf, &nobj);

			if(nobj == 0) {
				if(!keepworking) {
					if(wbuf)
						putempty(wbuf);
					goto endscan;
				}
				// Emptied our buffer: refill.
				wbuf = getfull(wbuf);
				if(wbuf == nil)
					goto endscan;
				nobj = wbuf->nobj;
				wp = wbuf->obj + wbuf->nobj;
			}
		}

		// Fetch b from the work buffer.
		--wp;
		b = wp->p;
		n = wp->n;
		ti = wp->ti;
		nobj--;
	}

endscan:
	runtime·lock(&lock);
	scanbuffers->next = bufferList;
	bufferList = scanbuffers;
	runtime·unlock(&lock);
}

// debug_scanblock is the debug copy of scanblock.
// it is simpler, slower, single-threaded, recursive,
// and uses bitSpecial as the mark bit.
/* 
   首先要将传入的地址,按机器字节大小对应.
   然后对待扫描区域的每个地址:
   找到它所在的MSpan,再找到该地址在MSpan中所处的对象地址.
   既然有了对象的地址,则根据它找到对应位图里的标记位.
   判断标记位,如果是未分配则跳过.否则打上特殊位标记(debug中用特殊位代码的mark位)
   还要判断是否含有无指针的标记位,如果没有,则还要递归地调用debug_scanblock.
*/
static void
debug_scanblock(byte *b, uintptr n)
{
	byte *obj, *p;
	void **vp;
	uintptr size, *bitp, bits, shift, i, xbits, off;
	MSpan *s;

	if(!DebugMark)
		runtime·throw("debug_scanblock without DebugMark");

	if((intptr)n < 0) {
		runtime·printf("debug_scanblock %p %D\n", b, (int64)n);
		runtime·throw("debug_scanblock");
	}

	// Align b to a word boundary.
	off = (uintptr)b & (PtrSize-1);
	if(off != 0) {
		b += PtrSize - off;
		n -= PtrSize - off;
	}

	vp = (void**)b;
	n /= PtrSize;
	for(i=0; i<n; i++) {
		obj = (byte*)vp[i];

		// Words outside the arena cannot be pointers.
		if((byte*)obj < runtime·mheap->arena_start || (byte*)obj >= runtime·mheap->arena_used)
			continue;

		// Round down to word boundary.
		obj = (void*)((uintptr)obj & ~((uintptr)PtrSize-1));

		// Consult span table to find beginning.
		s = runtime·MHeap_LookupMaybe(runtime·mheap, obj);
		if(s == nil)
			continue;

		/* MSpan的页的起始地址 */
		p =  (byte*)((uintptr)s->start<<PageShift);
		size = s->elemsize;
		if(s->sizeclass == 0) {
			obj = p;
		} else {
			if((byte*)obj >= (byte*)s->limit)
				continue;
			int32 i = ((byte*)obj - p)/size;
			obj = p+i*size;/* 从字对齐地址找到obj对齐的地址 */
		}

		// Now that we know the object header, reload bits.
		off = (uintptr*)obj - (uintptr*)runtime·mheap->arena_start;
		bitp = (uintptr*)runtime·mheap->arena_start - off/wordsPerBitmapWord - 1;
		shift = off % wordsPerBitmapWord;
		xbits = *bitp;
		bits = xbits >> shift;

		// Now we have bits, bitp, and shift correct for
		// obj pointing at the base of the object.
		// If not allocated or already marked, done.
		if((bits & bitAllocated) == 0 || (bits & bitSpecial) != 0)  // NOTE: bitSpecial not bitMarked
			continue;
		*bitp |= bitSpecial<<shift;
		if(!(bits & bitMarked))
			runtime·printf("found unmarked block %p in %p\n", obj, vp+i);

		// If object has no pointers, don't need to scan further.
		if((bits & bitNoPointers) != 0)
			continue;

		debug_scanblock(obj, size);
	}
}

// Append obj to the work buffer.
// _wbuf, _wp, _nobj are input/output parameters and are specifying the work buffer.
static void
enqueue(Obj obj, Workbuf **_wbuf, Obj **_wp, uintptr *_nobj)
{
	uintptr nobj, off;
	Obj *wp;
	Workbuf *wbuf;

	if(Debug > 1)
		runtime·printf("append obj(%p %D %p)\n", obj.p, (int64)obj.n, obj.ti);

	// Align obj.b to a word boundary.
	off = (uintptr)obj.p & (PtrSize-1);
	if(off != 0) {
		obj.p += PtrSize - off;
		obj.n -= PtrSize - off;
		obj.ti = 0;
	}

	if(obj.p == nil || obj.n == 0)
		return;

	// Load work buffer state
	wp = *_wp;
	wbuf = *_wbuf;
	nobj = *_nobj;

	// If another proc wants a pointer, give it some.
	if(work.nwait > 0 && nobj > handoffThreshold && work.full == 0) {
		wbuf->nobj = nobj;
		wbuf = handoff(wbuf);
		nobj = wbuf->nobj;
		wp = wbuf->obj + nobj;
	}

	// If buffer is full, get a new one.
	if(wbuf == nil || nobj >= nelem(wbuf->obj)) {
		if(wbuf != nil)
			wbuf->nobj = nobj;
		wbuf = getempty(wbuf);
		wp = wbuf->obj;
		nobj = 0;
	}

	*wp = obj;
	wp++;
	nobj++;

	// Save work buffer state
	*_wp = wp;
	*_wbuf = wbuf;
	*_nobj = nobj;
}

static void
markroot(ParFor *desc, uint32 i)
{
	Obj *wp;
	Workbuf *wbuf;
	uintptr nobj;

	USED(&desc);
	wp = nil;
	wbuf = nil;
	nobj = 0;
	enqueue(work.roots[i], &wbuf, &wp, &nobj);
	scanblock(wbuf, wp, nobj, false);
}

// Get an empty work buffer off the work.empty list,
// allocating new buffers as needed.
static Workbuf*
getempty(Workbuf *b)
{
	if(b != nil)
		runtime·lfstackpush(&work.full, &b->node);
	b = (Workbuf*)runtime·lfstackpop(&work.empty);
	if(b == nil) {
		// Need to allocate.
		runtime·lock(&work);
		if(work.nchunk < sizeof *b) {
			work.nchunk = 1<<20;
			work.chunk = runtime·SysAlloc(work.nchunk);
			if(work.chunk == nil)
				runtime·throw("runtime: cannot allocate memory");
		}
		b = (Workbuf*)work.chunk;
		work.chunk += sizeof *b;
		work.nchunk -= sizeof *b;
		runtime·unlock(&work);
	}
	b->nobj = 0;
	return b;
}

static void
putempty(Workbuf *b)
{
	if(CollectStats)
		runtime·xadd64(&gcstats.putempty, 1);

	runtime·lfstackpush(&work.empty, &b->node);
}

// Get a full work buffer off the work.full list, or return nil.
static Workbuf*
getfull(Workbuf *b)
{
	int32 i;

	if(CollectStats)
		runtime·xadd64(&gcstats.getfull, 1);

	if(b != nil)
		runtime·lfstackpush(&work.empty, &b->node);
	b = (Workbuf*)runtime·lfstackpop(&work.full);
	if(b != nil || work.nproc == 1)
		return b;

	runtime·xadd(&work.nwait, +1);
	for(i=0;; i++) {
		if(work.full != 0) {
			runtime·xadd(&work.nwait, -1);
			b = (Workbuf*)runtime·lfstackpop(&work.full);
			if(b != nil)
				return b;
			runtime·xadd(&work.nwait, +1);
		}
		if(work.nwait == work.nproc)
			return nil;
		if(i < 10) {
			m->gcstats.nprocyield++;
			runtime·procyield(20);
		} else if(i < 20) {
			m->gcstats.nosyield++;
			runtime·osyield();
		} else {
			m->gcstats.nsleep++;
			runtime·usleep(100);
		}
	}
}

static Workbuf*
handoff(Workbuf *b)
{
	int32 n;
	Workbuf *b1;

	// Make new buffer with half of b's pointers.
	b1 = getempty(nil);
	n = b->nobj/2;
	b->nobj -= n;
	b1->nobj = n;
	runtime·memmove(b1->obj, b->obj+b->nobj, n*sizeof b1->obj[0]);
	m->gcstats.nhandoff++;
	m->gcstats.nhandoffcnt += n;

	// Put b on full list - let first half of b get stolen.
	runtime·lfstackpush(&work.full, &b->node);
	return b1;
}

static void
addroot(Obj obj)
{
	uint32 cap;
	Obj *new;

	if(work.nroot >= work.rootcap) {
		cap = PageSize/sizeof(Obj);
		if(cap < 2*work.rootcap)
			cap = 2*work.rootcap;
		new = (Obj*)runtime·SysAlloc(cap*sizeof(Obj));
		if(new == nil)
			runtime·throw("runtime: cannot allocate memory");
		if(work.roots != nil) {
			runtime·memmove(new, work.roots, work.rootcap*sizeof(Obj));
			runtime·SysFree(work.roots, work.rootcap*sizeof(Obj));
		}
		work.roots = new;
		work.rootcap = cap;
	}
	work.roots[work.nroot] = obj;
	work.nroot++;
}

static void
addframeroots(Func *f, byte*, byte *sp, void*)
{
	if (f->frame > sizeof(uintptr))
		addroot((Obj){sp, f->frame - sizeof(uintptr), 0});
	if (f->args > 0)
		addroot((Obj){sp + f->frame, f->args, 0});
}

static void
addstackroots(G *gp)
{
	M *mp;
	Func *f;
	byte *sp, *pc;

	if(gp == g) {
		// Scanning our own stack: start at &gp.
		sp = (byte*)&gp;
		pc = runtime·getcallerpc(&gp);
	} else if((mp = gp->m) != nil && mp->helpgc) {
		// gchelper's stack is in active use and has no interesting pointers.
		return;
	} else if(gp->gcstack != (uintptr)nil) {
		// Scanning another goroutine that is about to enter or might
		// have just exited a system call. It may be executing code such
		// as schedlock and may have needed to start a new stack segment.
		// Use the stack segment and stack pointer at the time of
		// the system call instead, since that won't change underfoot.
		sp = (byte*)gp->gcsp;
		pc = gp->gcpc;
	} else {
		// Scanning another goroutine's stack.
		// The goroutine is usually asleep (the world is stopped).
		sp = (byte*)gp->sched.sp;
		pc = gp->sched.pc;
		if (pc == (byte*)runtime·goexit && gp->fnstart != nil) {
			// The goroutine has not started.  Its incoming
			// arguments are at the top of the stack and must
			// be scanned.  No other data on the stack.
			f = runtime·findfunc((uintptr)gp->fnstart->fn);
			if (f->args > 0)
				addroot((Obj){sp, f->args, 0});
			return;
		}
	}
	runtime·gentraceback(pc, sp, nil, gp, 0, nil, 0x7fffffff, addframeroots, nil);
}

static void
addfinroots(void *v)
{
	uintptr size;
	void *base;

	size = 0;
	if(!runtime·mlookup(v, &base, &size, nil) || !runtime·blockspecial(base))
		runtime·throw("mark - finalizer inconsistency");

	// do not mark the finalizer block itself.  just mark the things it points at.
	addroot((Obj){base, size, 0});
}

static void
addroots(void)
{
	G *gp;
	FinBlock *fb;
	MSpan *s, **allspans;
	uint32 spanidx;

	work.nroot = 0;

	// data & bss
	// TODO(atom): load balancing
	addroot((Obj){data, edata - data, (uintptr)gcdata});
	addroot((Obj){bss, ebss - bss, (uintptr)gcbss});

	// MSpan.types
	allspans = runtime·mheap->allspans;
	for(spanidx=0; spanidx<runtime·mheap->nspan; spanidx++) {
		s = allspans[spanidx];
		if(s->state == MSpanInUse) {
			switch(s->types.compression) {
			case MTypes_Empty:
			case MTypes_Single:
				break;
			case MTypes_Words:
			case MTypes_Bytes:
				// TODO(atom): consider using defaultProg instead of 0
				addroot((Obj){(byte*)&s->types.data, sizeof(void*), 0});
				break;
			}
		}
	}

	// stacks
	for(gp=runtime·allg; gp!=nil; gp=gp->alllink) {
		switch(gp->status){
		default:
			runtime·printf("unexpected G.status %d\n", gp->status);
			runtime·throw("mark - bad status");
		case Gdead:
			break;
		case Grunning:
			if(gp != g)
				runtime·throw("mark - world not stopped");
			addstackroots(gp);
			break;
		case Grunnable:
		case Gsyscall:
		case Gwaiting:
			addstackroots(gp);
			break;
		}
	}

	runtime·walkfintab(addfinroots);

	for(fb=allfin; fb; fb=fb->alllink)
		addroot((Obj){(byte*)fb->fin, fb->cnt*sizeof(fb->fin[0]), 0});
}

static bool
handlespecial(byte *p, uintptr size)
{
	FuncVal *fn;
	uintptr nret;
	FinBlock *block;
	Finalizer *f;

	if(!runtime·getfinalizer(p, true, &fn, &nret)) {
		runtime·setblockspecial(p, false);
		runtime·MProf_Free(p, size);
		return false;
	}

	runtime·lock(&finlock);
	if(finq == nil || finq->cnt == finq->cap) {
		if(finc == nil) {
			finc = runtime·SysAlloc(PageSize);
			if(finc == nil)
				runtime·throw("runtime: cannot allocate memory");
			finc->cap = (PageSize - sizeof(FinBlock)) / sizeof(Finalizer) + 1;
			finc->alllink = allfin;
			allfin = finc;
		}
		block = finc;
		finc = block->next;
		block->next = finq;
		finq = block;
	}
	f = &finq->fin[finq->cnt];
	finq->cnt++;
	f->fn = fn;
	f->nret = nret;
	f->arg = p;
	runtime·unlock(&finlock);
	return true;
}

// Sweep frees or collects finalizers for blocks not marked in the mark phase.
// It clears the mark bits in preparation for the next GC round.
static void
sweepspan(ParFor *desc, uint32 idx)
{
	int32 cl, n, npages;
	uintptr size;
	byte *p;
	MCache *c;
	byte *arena_start;
	MLink head, *end;
	int32 nfree;
	byte *type_data;
	byte compression;
	uintptr type_data_inc;
	MSpan *s;

	USED(&desc);
	s = runtime·mheap->allspans[idx];
	if(s->state != MSpanInUse)
		return;
	arena_start = runtime·mheap->arena_start;
	p = (byte*)(s->start << PageShift);
	cl = s->sizeclass;
	size = s->elemsize;
	if(cl == 0) {
		n = 1;
	} else {
		// Chunk full of small blocks.
		npages = runtime·class_to_allocnpages[cl];
		n = (npages << PageShift) / size;
	}
	nfree = 0;
	end = &head;
	c = m->mcache;
	
	type_data = (byte*)s->types.data;
	type_data_inc = sizeof(uintptr);
	compression = s->types.compression;
	switch(compression) {
	case MTypes_Bytes:
		type_data += 8*sizeof(uintptr);
		type_data_inc = 1;
		break;
	}

	// Sweep through n objects of given size starting at p.
	// This thread owns the span now, so it can manipulate
	// the block bitmap without atomic operations.
	for(; n > 0; n--, p += size, type_data+=type_data_inc) {
		uintptr off, *bitp, shift, bits;

		off = (uintptr*)p - (uintptr*)arena_start;
		bitp = (uintptr*)arena_start - off/wordsPerBitmapWord - 1;
		shift = off % wordsPerBitmapWord;
		bits = *bitp>>shift;

		if((bits & bitAllocated) == 0)
			continue;

		if((bits & bitMarked) != 0) {
			if(DebugMark) {
				if(!(bits & bitSpecial))
					runtime·printf("found spurious mark on %p\n", p);
				*bitp &= ~(bitSpecial<<shift);
			}
			*bitp &= ~(bitMarked<<shift);
			continue;
		}

		// Special means it has a finalizer or is being profiled.
		// In DebugMark mode, the bit has been coopted so
		// we have to assume all blocks are special.
		if(DebugMark || (bits & bitSpecial) != 0) {
			if(handlespecial(p, size))
				continue;
		}

		// Mark freed; restore block boundary bit.
		*bitp = (*bitp & ~(bitMask<<shift)) | (bitBlockBoundary<<shift);

		if(cl == 0) {
			// Free large span.
			runtime·unmarkspan(p, 1<<PageShift);
			*(uintptr*)p = 1;	// needs zeroing
			runtime·MHeap_Free(runtime·mheap, s, 1);
			c->local_alloc -= size;
			c->local_nfree++;
		} else {
			// Free small object.
			switch(compression) {
			case MTypes_Words:
				*(uintptr*)type_data = 0;
				break;
			case MTypes_Bytes:
				*(byte*)type_data = 0;
				break;
			}
			if(size > sizeof(uintptr))
				((uintptr*)p)[1] = 1;	// mark as "needs to be zeroed"
			
			end->next = (MLink*)p;
			end = (MLink*)p;
			nfree++;
		}
	}

	if(nfree) {
		c->local_by_size[cl].nfree += nfree;
		c->local_alloc -= size * nfree;
		c->local_nfree += nfree;
		c->local_cachealloc -= nfree * size;
		c->local_objects -= nfree;
		runtime·MCentral_FreeSpan(&runtime·mheap->central[cl], s, nfree, head.next, end);
	}
}

static void
dumpspan(uint32 idx)
{
	int32 sizeclass, n, npages, i, column;
	uintptr size;
	byte *p;
	byte *arena_start;
	MSpan *s;
	bool allocated, special;

	s = runtime·mheap->allspans[idx];
	if(s->state != MSpanInUse)
		return;
	arena_start = runtime·mheap->arena_start;
	p = (byte*)(s->start << PageShift);
	sizeclass = s->sizeclass;
	size = s->elemsize;
	if(sizeclass == 0) {
		n = 1;
	} else {
		npages = runtime·class_to_allocnpages[sizeclass];
		n = (npages << PageShift) / size;
	}
	
	runtime·printf("%p .. %p:\n", p, p+n*size);
	column = 0;
	for(; n>0; n--, p+=size) {
		uintptr off, *bitp, shift, bits;

		off = (uintptr*)p - (uintptr*)arena_start;
		bitp = (uintptr*)arena_start - off/wordsPerBitmapWord - 1;
		shift = off % wordsPerBitmapWord;
		bits = *bitp>>shift;

		allocated = ((bits & bitAllocated) != 0);
		special = ((bits & bitSpecial) != 0);

		for(i=0; i<size; i+=sizeof(void*)) {
			if(column == 0) {
				runtime·printf("\t");
			}
			if(i == 0) {
				runtime·printf(allocated ? "(" : "[");
				runtime·printf(special ? "@" : "");
				runtime·printf("%p: ", p+i);
			} else {
				runtime·printf(" ");
			}

			runtime·printf("%p", *(void**)(p+i));

			if(i+sizeof(void*) >= size) {
				runtime·printf(allocated ? ") " : "] ");
			}

			column++;
			if(column == 8) {
				runtime·printf("\n");
				column = 0;
			}
		}
	}
	runtime·printf("\n");
}

// A debugging function to dump the contents of memory
void
runtime·memorydump(void)
{
	uint32 spanidx;

	for(spanidx=0; spanidx<runtime·mheap->nspan; spanidx++) {
		dumpspan(spanidx);
	}
}

void
runtime·gchelper(void)
{
	// parallel mark for over gc roots
	runtime·parfordo(work.markfor);

	// help other threads scan secondary blocks
	scanblock(nil, nil, 0, true);

	if(DebugMark) {
		// wait while the main thread executes mark(debug_scanblock)
		while(runtime·atomicload(&work.debugmarkdone) == 0)
			runtime·usleep(10);
	}

	runtime·parfordo(work.sweepfor);
	if(runtime·xadd(&work.ndone, +1) == work.nproc-1)
		runtime·notewakeup(&work.alldone);
}

#define GcpercentUnknown (-2)

// Initialized from $GOGC.  GOGC=off means no gc.
//
// Next gc is after we've allocated an extra amount of
// memory proportional to the amount already in use.
// If gcpercent=100 and we're using 4M, we'll gc again
// when we get to 8M.  This keeps the gc cost in linear
// proportion to the allocation cost.  Adjusting gcpercent
// just changes the linear constant (and also the amount of
// extra memory used).
static int32 gcpercent = GcpercentUnknown;

static void
cachestats(GCStats *stats)
{
	M *mp;
	MCache *c;
	P *p, **pp;
	int32 i;
	uint64 stacks_inuse;
	uint64 *src, *dst;

	if(stats)
		runtime·memclr((byte*)stats, sizeof(*stats));
	stacks_inuse = 0;
	for(mp=runtime·allm; mp; mp=mp->alllink) {
		stacks_inuse += mp->stackinuse*FixedStack;
		if(stats) {
			src = (uint64*)&mp->gcstats;
			dst = (uint64*)stats;
			for(i=0; i<sizeof(*stats)/sizeof(uint64); i++)
				dst[i] += src[i];
			runtime·memclr((byte*)&mp->gcstats, sizeof(mp->gcstats));
		}
	}
	for(pp=runtime·allp; p=*pp; pp++) {
		c = p->mcache;
		if(c==nil)
			continue;
		runtime·purgecachedstats(c);
		for(i=0; i<nelem(c->local_by_size); i++) {
			mstats.by_size[i].nmalloc += c->local_by_size[i].nmalloc;
			c->local_by_size[i].nmalloc = 0;
			mstats.by_size[i].nfree += c->local_by_size[i].nfree;
			c->local_by_size[i].nfree = 0;
		}
	}
	mstats.stacks_inuse = stacks_inuse;
}

// Structure of arguments passed to function gc().
// This allows the arguments to be passed via reflect·call.
struct gc_args
{
	int32 force;
};

static void gc(struct gc_args *args);

static int32
readgogc(void)
{
	byte *p;

	p = runtime·getenv("GOGC");
	if(p == nil || p[0] == '\0')
		return 100;
	if(runtime·strcmp(p, (byte*)"off") == 0)
		return -1;
	return runtime·atoi(p);
}

void
runtime·gc(int32 force)
{
	byte *p;
	struct gc_args a, *ap;
	FuncVal gcv;

	// The atomic operations are not atomic if the uint64s
	// are not aligned on uint64 boundaries. This has been
	// a problem in the past.
	if((((uintptr)&work.empty) & 7) != 0)
		runtime·throw("runtime: gc work buffer is misaligned");

	// The gc is turned off (via enablegc) until
	// the bootstrap has completed.
	// Also, malloc gets called in the guts
	// of a number of libraries that might be
	// holding locks.  To avoid priority inversion
	// problems, don't bother trying to run gc
	// while holding a lock.  The next mallocgc
	// without a lock will do the gc instead.
	if(!mstats.enablegc || m->locks > 0 || runtime·panicking)
		return;

	if(gcpercent == GcpercentUnknown) {	// first time through
		gcpercent = readgogc();

		p = runtime·getenv("GOGCTRACE");
		if(p != nil)
			gctrace = runtime·atoi(p);
	}
	if(gcpercent < 0)
		return;

	// Run gc on a bigger stack to eliminate
	// a potentially large number of calls to runtime·morestack.
	a.force = force;
	ap = &a;
	m->moreframesize_minalloc = StackBig;
	gcv.fn = (void*)gc;
	reflect·call(&gcv, (byte*)&ap, sizeof(ap));

	if(gctrace > 1 && !force) {
		a.force = 1;
		gc(&a);
	}
}

static FuncVal runfinqv = {runfinq};

static void
gc(struct gc_args *args)
{
	int64 t0, t1, t2, t3, t4;
	uint64 heap0, heap1, obj0, obj1, ninstr;
	GCStats stats;
	M *mp;
	uint32 i;
	Eface eface;

	//通过semacquire之后才授权m可以stoptheworld
	runtime·semacquire(&runtime·worldsema);
	if(!args->force && mstats.heap_alloc < mstats.next_gc) {
		runtime·semrelease(&runtime·worldsema);
		return;
	}

	t0 = runtime·nanotime();

	//停止所有goroutine的运行
	m->gcing = 1;
	runtime·stoptheworld();

	if(CollectStats)
		runtime·memclr((byte*)&gcstats, sizeof(gcstats));

	//设置了各个m中的MSpan中的位图的一些标记位
	for(mp=runtime·allm; mp; mp=mp->alllink)
		runtime·settype_flush(mp, false);

	heap0 = 0;
	obj0 = 0;
	if(gctrace) {
		cachestats(nil);
		heap0 = mstats.heap_alloc;
		obj0 = mstats.nmalloc - mstats.nfree;
	}

	m->locks++;	// disable gc during mallocs in parforalloc
	if(work.markfor == nil)
		work.markfor = runtime·parforalloc(MaxGcproc);
	if(work.sweepfor == nil)
		work.sweepfor = runtime·parforalloc(MaxGcproc);
	m->locks--;

	if(itabtype == nil) {
		// get C pointer to the Go type "itab"
		runtime·gc_itab_ptr(&eface);
		itabtype = ((PtrType*)eface.type)->elem;
	}

	work.nwait = 0;
	work.ndone = 0;
	work.debugmarkdone = 0;
	work.nproc = runtime·gcprocs();
	//添加垃圾回收的roots
	addroots();
	//设置并行任务，它们被启动后分别会运行markroot和sweepspan
	runtime·parforsetup(work.markfor, work.nproc, work.nroot, nil, false, markroot);
	runtime·parforsetup(work.sweepfor, work.nproc, runtime·mheap->nspan, nil, true, sweepspan);
	if(work.nproc > 1) {
		runtime·noteclear(&work.alldone);
		runtime·helpgc(work.nproc);
	}

	t1 = runtime·nanotime();

	//执行mark任务，如果有多条线程，它们将会并行的执行markroot函数
	runtime·parfordo(work.markfor);
	scanblock(nil, nil, 0, true);

	if(DebugMark) {
		for(i=0; i<work.nroot; i++)
			debug_scanblock(work.roots[i].p, work.roots[i].n);
		runtime·atomicstore(&work.debugmarkdone, 1);
	}
	t2 = runtime·nanotime();

	//执行sweep任务，如果有多条线程，它们将会并行的执行sweepspan函数
	runtime·parfordo(work.sweepfor);
	t3 = runtime·nanotime();

	if(work.nproc > 1)
		runtime·notesleep(&work.alldone);

	cachestats(&stats);

	stats.nprocyield += work.sweepfor->nprocyield;
	stats.nosyield += work.sweepfor->nosyield;
	stats.nsleep += work.sweepfor->nsleep;

	mstats.next_gc = mstats.heap_alloc+mstats.heap_alloc*gcpercent/100;
	m->gcing = 0;

	if(finq != nil) {
		m->locks++;	// disable gc during the mallocs in newproc
		// kick off or wake up goroutine to run queued finalizers
		if(fing == nil)
			fing = runtime·newproc1(&runfinqv, nil, 0, 0, runtime·gc);
		else if(fingwait) {
			fingwait = 0;
			runtime·ready(fing);
		}
		m->locks--;
	}

	heap1 = mstats.heap_alloc;
	obj1 = mstats.nmalloc - mstats.nfree;

	t4 = runtime·nanotime();
	mstats.last_gc = t4;
	mstats.pause_ns[mstats.numgc%nelem(mstats.pause_ns)] = t4 - t0;
	mstats.pause_total_ns += t4 - t0;
	mstats.numgc++;
	if(mstats.debuggc)
		runtime·printf("pause %D\n", t4-t0);

	if(gctrace) {
		runtime·printf("gc%d(%d): %D+%D+%D ms, %D -> %D MB %D -> %D (%D-%D) objects,"
				" %D(%D) handoff, %D(%D) steal, %D/%D/%D yields\n",
			mstats.numgc, work.nproc, (t2-t1)/1000000, (t3-t2)/1000000, (t1-t0+t4-t3)/1000000,
			heap0>>20, heap1>>20, obj0, obj1,
			mstats.nmalloc, mstats.nfree,
			stats.nhandoff, stats.nhandoffcnt,
			work.sweepfor->nsteal, work.sweepfor->nstealcnt,
			stats.nprocyield, stats.nosyield, stats.nsleep);
		if(CollectStats) {
			runtime·printf("scan: %D bytes, %D objects, %D untyped, %D types from MSpan\n",
				gcstats.nbytes, gcstats.obj.cnt, gcstats.obj.notype, gcstats.obj.typelookup);
			if(gcstats.ptr.cnt != 0)
				runtime·printf("avg ptrbufsize: %D (%D/%D)\n",
					gcstats.ptr.sum/gcstats.ptr.cnt, gcstats.ptr.sum, gcstats.ptr.cnt);
			if(gcstats.obj.cnt != 0)
				runtime·printf("avg nobj: %D (%D/%D)\n",
					gcstats.obj.sum/gcstats.obj.cnt, gcstats.obj.sum, gcstats.obj.cnt);
			runtime·printf("rescans: %D, %D bytes\n", gcstats.rescan, gcstats.rescanbytes);

			runtime·printf("instruction counts:\n");
			ninstr = 0;
			for(i=0; i<nelem(gcstats.instr); i++) {
				runtime·printf("\t%d:\t%D\n", i, gcstats.instr[i]);
				ninstr += gcstats.instr[i];
			}
			runtime·printf("\ttotal:\t%D\n", ninstr);

			runtime·printf("putempty: %D, getfull: %D\n", gcstats.putempty, gcstats.getfull);
		}
	}

	runtime·MProf_GC();
	runtime·semrelease(&runtime·worldsema);
	runtime·starttheworld();

	// give the queued finalizers, if any, a chance to run
	if(finq != nil)
		runtime·gosched();
}

void
runtime·ReadMemStats(MStats *stats)
{
	// Have to acquire worldsema to stop the world,
	// because stoptheworld can only be used by
	// one goroutine at a time, and there might be
	// a pending garbage collection already calling it.
	runtime·semacquire(&runtime·worldsema);
	m->gcing = 1;
	runtime·stoptheworld();
	cachestats(nil);
	*stats = mstats;
	m->gcing = 0;
	runtime·semrelease(&runtime·worldsema);
	runtime·starttheworld();
}

void
runtime∕debug·readGCStats(Slice *pauses)
{
	uint64 *p;
	uint32 i, n;

	// Calling code in runtime/debug should make the slice large enough.
	if(pauses->cap < nelem(mstats.pause_ns)+3)
		runtime·throw("runtime: short slice passed to readGCStats");

	// Pass back: pauses, last gc (absolute time), number of gc, total pause ns.
	p = (uint64*)pauses->array;
	runtime·lock(runtime·mheap);
	n = mstats.numgc;
	if(n > nelem(mstats.pause_ns))
		n = nelem(mstats.pause_ns);
	
	// The pause buffer is circular. The most recent pause is at
	// pause_ns[(numgc-1)%nelem(pause_ns)], and then backward
	// from there to go back farther in time. We deliver the times
	// most recent first (in p[0]).
	for(i=0; i<n; i++)
		p[i] = mstats.pause_ns[(mstats.numgc-1-i)%nelem(mstats.pause_ns)];

	p[n] = mstats.last_gc;
	p[n+1] = mstats.numgc;
	p[n+2] = mstats.pause_total_ns;	
	runtime·unlock(runtime·mheap);
	pauses->len = n+3;
}

void
runtime∕debug·setGCPercent(intgo in, intgo out)
{
	runtime·lock(runtime·mheap);
	if(gcpercent == GcpercentUnknown)
		gcpercent = readgogc();
	out = gcpercent;
	if(in < 0)
		in = -1;
	gcpercent = in;
	runtime·unlock(runtime·mheap);
	FLUSH(&out);
}

static void
runfinq(void)
{
	Finalizer *f;
	FinBlock *fb, *next;
	byte *frame;
	uint32 framesz, framecap, i;

	frame = nil;
	framecap = 0;
	for(;;) {
		// There's no need for a lock in this section
		// because it only conflicts with the garbage
		// collector, and the garbage collector only
		// runs when everyone else is stopped, and
		// runfinq only stops at the gosched() or
		// during the calls in the for loop.
		fb = finq;
		finq = nil;
		if(fb == nil) {
			fingwait = 1;
			runtime·park(nil, nil, "finalizer wait");
			continue;
		}
		if(raceenabled)
			runtime·racefingo();
		for(; fb; fb=next) {
			next = fb->next;
			for(i=0; i<fb->cnt; i++) {
				f = &fb->fin[i];
				framesz = sizeof(uintptr) + f->nret;
				if(framecap < framesz) {
					runtime·free(frame);
					frame = runtime·mal(framesz);
					framecap = framesz;
				}
				*(void**)frame = f->arg;
				reflect·call(f->fn, frame, sizeof(uintptr) + f->nret);
				f->fn = nil;
				f->arg = nil;
			}
			fb->cnt = 0;
			fb->next = finc;
			finc = fb;
		}
		runtime·gc(1);	// trigger another gc to clean up the finalized objects, if possible
	}
}

// mark the block at v of size n as allocated.
// If noptr is true, mark it as having no pointers.
void
runtime·markallocated(void *v, uintptr n, bool noptr)
{
	uintptr *b, obits, bits, off, shift;

	if(0)
		runtime·printf("markallocated %p+%p\n", v, n);
	/* 确定在这个区域之内 */
	if((byte*)v+n > (byte*)runtime·mheap->arena_used || (byte*)v < runtime·mheap->arena_start)
		runtime·throw("markallocated: bad pointer");
	/*计算偏移 */
	off = (uintptr*)v - (uintptr*)runtime·mheap->arena_start;  // word offset
	/* 根据偏移找到标记位图 */
	b = (uintptr*)runtime·mheap->arena_start - off/wordsPerBitmapWord - 1;
	shift = off % wordsPerBitmapWord;

	/* 将对应的位全部加上标记 */
	for(;;) {
		obits = *b;
		bits = (obits & ~(bitMask<<shift)) | (bitAllocated<<shift);
		if(noptr)
			bits |= bitNoPointers<<shift;
		if(runtime·singleproc) {
			*b = bits;
			break;
		} else {
			// more than one goroutine is potentially running: use atomic op
			if(runtime·casp((void**)b, (void*)obits, (void*)bits))
				break;
		}
	}
}

// mark the block at v of size n as freed.
void
runtime·markfreed(void *v, uintptr n)
{
	uintptr *b, obits, bits, off, shift;

	if(0)
		runtime·printf("markallocated %p+%p\n", v, n);

	if((byte*)v+n > (byte*)runtime·mheap->arena_used || (byte*)v < runtime·mheap->arena_start)
		runtime·throw("markallocated: bad pointer");

	off = (uintptr*)v - (uintptr*)runtime·mheap->arena_start;  // word offset
	b = (uintptr*)runtime·mheap->arena_start - off/wordsPerBitmapWord - 1;
	shift = off % wordsPerBitmapWord;

	for(;;) {
		obits = *b;
		bits = (obits & ~(bitMask<<shift)) | (bitBlockBoundary<<shift);
		if(runtime·singleproc) {
			*b = bits;
			break;
		} else {
			// more than one goroutine is potentially running: use atomic op
			if(runtime·casp((void**)b, (void*)obits, (void*)bits))
				break;
		}
	}
}

// check that the block at v of size n is marked freed.
void
runtime·checkfreed(void *v, uintptr n)
{
	uintptr *b, bits, off, shift;

	if(!runtime·checking)
		return;

	if((byte*)v+n > (byte*)runtime·mheap->arena_used || (byte*)v < runtime·mheap->arena_start)
		return;	// not allocated, so okay

	off = (uintptr*)v - (uintptr*)runtime·mheap->arena_start;  // word offset
	b = (uintptr*)runtime·mheap->arena_start - off/wordsPerBitmapWord - 1;
	shift = off % wordsPerBitmapWord;

	bits = *b>>shift;
	if((bits & bitAllocated) != 0) {
		runtime·printf("checkfreed %p+%p: off=%p have=%p\n",
			v, n, off, bits & bitMask);
		runtime·throw("checkfreed: not freed");
	}
}

// mark the span of memory at v as having n blocks of the given size.
// if leftover is true, there is left over space at the end of the span.
void
runtime·markspan(void *v, uintptr size, uintptr n, bool leftover)
{
	uintptr *b, off, shift;
	byte *p;

	if((byte*)v+size*n > (byte*)runtime·mheap->arena_used || (byte*)v < runtime·mheap->arena_start)
		runtime·throw("markspan: bad pointer");

	p = v;
	if(leftover)	// mark a boundary just past end of last block too
		n++;
	for(; n-- > 0; p += size) {
		// Okay to use non-atomic ops here, because we control
		// the entire span, and each bitmap word has bits for only
		// one span, so no other goroutines are changing these
		// bitmap words.
		off = (uintptr*)p - (uintptr*)runtime·mheap->arena_start;  // word offset
		b = (uintptr*)runtime·mheap->arena_start - off/wordsPerBitmapWord - 1;
		shift = off % wordsPerBitmapWord;
		*b = (*b & ~(bitMask<<shift)) | (bitBlockBoundary<<shift);
	}
}

// unmark the span of memory at v of length n bytes.
void
runtime·unmarkspan(void *v, uintptr n)
{
	uintptr *p, *b, off;

	if((byte*)v+n > (byte*)runtime·mheap->arena_used || (byte*)v < runtime·mheap->arena_start)
		runtime·throw("markspan: bad pointer");

	p = v;
	off = p - (uintptr*)runtime·mheap->arena_start;  // word offset
	if(off % wordsPerBitmapWord != 0)
		runtime·throw("markspan: unaligned pointer");
	b = (uintptr*)runtime·mheap->arena_start - off/wordsPerBitmapWord - 1;
	n /= PtrSize;
	if(n%wordsPerBitmapWord != 0)
		runtime·throw("unmarkspan: unaligned length");
	// Okay to use non-atomic ops here, because we control
	// the entire span, and each bitmap word has bits for only
	// one span, so no other goroutines are changing these
	// bitmap words.
	n /= wordsPerBitmapWord;
	while(n-- > 0)
		*b-- = 0;
}

bool
runtime·blockspecial(void *v)
{
	uintptr *b, off, shift;

	if(DebugMark)
		return true;

	off = (uintptr*)v - (uintptr*)runtime·mheap->arena_start;
	b = (uintptr*)runtime·mheap->arena_start - off/wordsPerBitmapWord - 1;
	shift = off % wordsPerBitmapWord;

	return (*b & (bitSpecial<<shift)) != 0;
}

void
runtime·setblockspecial(void *v, bool s)
{
	uintptr *b, off, shift, bits, obits;

	if(DebugMark)
		return;

	off = (uintptr*)v - (uintptr*)runtime·mheap->arena_start;
	b = (uintptr*)runtime·mheap->arena_start - off/wordsPerBitmapWord - 1;
	shift = off % wordsPerBitmapWord;

	for(;;) {
		obits = *b;
		if(s)
			bits = obits | (bitSpecial<<shift);
		else
			bits = obits & ~(bitSpecial<<shift);
		if(runtime·singleproc) {
			*b = bits;
			break;
		} else {
			// more than one goroutine is potentially running: use atomic op
			if(runtime·casp((void**)b, (void*)obits, (void*)bits))
				break;
		}
	}
}

void
runtime·MHeap_MapBits(MHeap *h)
{
	// Caller has added extra mappings to the arena.
	// Add extra mappings of bitmap words as needed.
	// We allocate extra bitmap pieces in chunks of bitmapChunk.
	enum {
		bitmapChunk = 8192
	};
	uintptr n;

	n = (h->arena_used - h->arena_start) / wordsPerBitmapWord;
	n = (n+bitmapChunk-1) & ~(bitmapChunk-1);
	if(h->bitmap_mapped >= n)
		return;

	runtime·SysMap(h->arena_start - n, n - h->bitmap_mapped);
	h->bitmap_mapped = n;
}
